#include "lexus_ux.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#include <furi.h>
#include <string.h>
#include <stdlib.h>

/*
 * Lexus UX: the smart key of a 2021 Lexus UX 250 (European model),
 * DECODE ONLY. KAT's own decoder (github.com/KaraZajac/KAT, src/
 * protocols/lexus_ux.rs), worked out from two real recordings (five
 * presses each); no other reference reads this signal as recorded. NOT
 * the same decode as our lexus_v0.c: that one (ported from KeeLoq-RX's
 * arke_toyota.py) reads the first 224 bits of the exact same raw signal
 * directly as Manchester, without the differential step this file adds -
 * the two will show different, unrelated-looking bits for the same key.
 *
 * Air format: 2-FSK, Te=348.4us. This is DIFFERENTIAL Manchester: decode
 * it as plain Manchester first (one raw bit per cell, via this firmware's
 * manchester_decoder.c), then XOR each raw bit with the one before it -
 * that XOR result is the real frame bit. (The very first raw bit of a
 * burst has no predecessor, so it produces no frame bit - this decoder
 * just treats it as establishing the starting point, same as every other
 * Manchester protocol in this tree treats its first post-reset edge.) A
 * press sends the 256-bit frame 4 times, back to back, no gap - and
 * unlike kia_denso.c/kia_forte.c, no repeat-confirmation is needed: each
 * copy independently passes its own CRC, so every valid copy is reported
 * on its own.
 *
 * Frame (256 bits): every 8th bit from bit 8 on (8, 16, ..., 248) is a
 * fixed "1" stuffing bit, not data - bits 0..47 are a fixed header
 * (00 C4 80 80 88 80), bits 49..52 the button (0010 Lock, 0100 Unlock),
 * bits 53..198 a 128-bit rolling code (the stuffing bits removed - no
 * known cipher), bits 200..231 a 32-bit ID (kept WITH its own stuffing
 * bits, unlike the code), bits 236..253 a CRC-16 (CCITT polynomial 0x1021,
 * register 0, no reflection; computed over bits 10..235 WITH the stuffing
 * bits, but read back out with them removed - 18 raw bits holding 16 real
 * ones), and a closing "11" at bits 254..255.
 *
 * Only the first 64 of the code's 128 bits are kept here (as our `data`
 * field) - the same storage simplification used elsewhere in this tree
 * for fields wider than 64 bits (see microchip_hcs361.c).
 */

#define LEXUS_UX_TE 348u
#define LEXUS_UX_TOL 174u
#define LEXUS_UX_RESET_GAP 2000u
#define LEXUS_UX_FRAME_BITS 256u
#define LEXUS_UX_FRAME_BYTES 32u
#define LEXUS_UX_HEAD_SEARCH_BITS 47u /* bits 1..47 of the head (bit 0 is always 0 and unchecked) */
#define LEXUS_UX_CRC_POLY 0x1021u

#define LEXUS_UX_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < LEXUS_UX_TOL)

typedef enum {
    LexusUxStepSearching = 0,
    LexusUxStepCollecting,
} LexusUxDecoderStep;

struct SubGhzProtocolDecoderLexusUx {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    bool have_prev_manchester;
    bool prev_manchester_bit;
    uint64_t window; /* rolling last-47-bits window, for the head match */
    uint8_t frame[LEXUS_UX_FRAME_BYTES];
    uint16_t count;
};

static const char* const lexus_ux_names[2] = {"Lock", "Unlock"};

const SubGhzProtocolDecoder subghz_protocol_lexus_ux_decoder = {
    .alloc = subghz_protocol_decoder_lexus_ux_alloc,
    .free = subghz_protocol_decoder_lexus_ux_free,
    .reset = subghz_protocol_decoder_lexus_ux_reset,
    .feed = subghz_protocol_decoder_lexus_ux_feed,
    .get_hash_data = subghz_protocol_decoder_lexus_ux_get_hash_data,
    .serialize = subghz_protocol_decoder_lexus_ux_serialize,
    .deserialize = subghz_protocol_decoder_lexus_ux_deserialize,
    .get_string = subghz_protocol_decoder_lexus_ux_get_string,
};

/* Decode-only - the cipher is unknown. */
const SubGhzProtocolEncoder subghz_protocol_lexus_ux_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_lexus_ux = {
    .name = SUBGHZ_PROTOCOL_LEXUS_UX_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_lexus_ux_decoder,
    .encoder = &subghz_protocol_lexus_ux_encoder,
};

static inline bool lexus_ux_get_bit(const uint8_t* frame, int pos) {
    return (frame[pos / 8] >> (7 - (pos % 8))) & 1u;
}

/* Reads bits [start,end) as a plain MSB-first value, every bit included. */
static uint32_t lexus_ux_bits(const uint8_t* frame, int start, int end) {
    uint32_t v = 0;
    for(int i = start; i < end; i++) v = (v << 1) | (lexus_ux_get_bit(frame, i) ? 1u : 0u);
    return v;
}

/* Same, but skipping every position that is a multiple of 8 (a stuffing
 * bit, not data) - matches the reference's `unframed`. */
static uint32_t lexus_ux_unframed_bits(const uint8_t* frame, int start, int end) {
    uint32_t v = 0;
    for(int i = start; i < end; i++) {
        if(i % 8 == 0) continue;
        v = (v << 1) | (lexus_ux_get_bit(frame, i) ? 1u : 0u);
    }
    return v;
}

static uint16_t lexus_ux_crc16(const uint8_t* frame, int start, int end) {
    uint16_t crc = 0;
    for(int i = start; i < end; i++) {
        bool bit = lexus_ux_get_bit(frame, i);
        bool msb = (crc >> 15) & 1u;
        crc = (uint16_t)(crc << 1);
        if(msb != bit) crc ^= LEXUS_UX_CRC_POLY;
    }
    return crc;
}

static bool lexus_ux_fields(SubGhzBlockGeneric* generic, const uint8_t* frame) {
    for(int i = 8; i < (int)LEXUS_UX_FRAME_BITS; i += 8) {
        if(!lexus_ux_get_bit(frame, i)) return false;
    }
    if(!lexus_ux_get_bit(frame, 254) || !lexus_ux_get_bit(frame, 255)) return false;

    uint16_t computed = lexus_ux_crc16(frame, 10, 236);
    uint16_t stored = (uint16_t)lexus_ux_unframed_bits(frame, 236, 254);
    if(computed != stored) return false;

    generic->btn = (uint8_t)lexus_ux_bits(frame, 49, 53);
    generic->serial = lexus_ux_bits(frame, 200, 232);
    /* Only the first 64 of the code's 128 bits are kept (see file header). */
    uint64_t code_hi = 0;
    for(int i = 53, got = 0; i < 199 && got < 64; i++) {
        if(i % 8 == 0) continue;
        code_hi = (code_hi << 1) | (lexus_ux_get_bit(frame, i) ? 1u : 0u);
        got++;
    }
    generic->data = code_hi;
    generic->data_count_bit = 64;
    return true;
}

void* subghz_protocol_decoder_lexus_ux_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderLexusUx* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_lexus_ux;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_LEXUS_UX_NAME;
    return instance;
}

void subghz_protocol_decoder_lexus_ux_free(void* context) {
    furi_check(context);
    free(context);
}

static void lexus_ux_reset_search(SubGhzProtocolDecoderLexusUx* instance) {
    instance->decoder.parser_step = LexusUxStepSearching;
    instance->window = 0;
    instance->have_prev_manchester = false;
    instance->manchester_state = ManchesterStateMid1;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_lexus_ux_reset(void* context) {
    furi_check(context);
    lexus_ux_reset_search(context);
}

void subghz_protocol_decoder_lexus_ux_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderLexusUx* instance = context;

    if(duration > LEXUS_UX_RESET_GAP) {
        lexus_ux_reset_search(instance);
        return;
    }

    ManchesterEvent event;
    if(LEXUS_UX_IN_TOL(duration, LEXUS_UX_TE)) {
        event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    } else if(LEXUS_UX_IN_TOL(duration, 2u * LEXUS_UX_TE)) {
        event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    } else {
        lexus_ux_reset_search(instance);
        return;
    }

    bool mbit;
    if(!manchester_advance(instance->manchester_state, event, &instance->manchester_state, &mbit)) {
        return;
    }

    if(!instance->have_prev_manchester) {
        instance->prev_manchester_bit = mbit;
        instance->have_prev_manchester = true;
        return;
    }
    bool diff_bit = mbit != instance->prev_manchester_bit;
    instance->prev_manchester_bit = mbit;

    if(instance->decoder.parser_step == LexusUxStepCollecting) {
        if(diff_bit)
            instance->frame[instance->count / 8] |= (uint8_t)(0x80 >> (instance->count % 8));
        instance->count++;
        if(instance->count == LEXUS_UX_FRAME_BITS) {
            bool valid = lexus_ux_fields(&instance->generic, instance->frame);
            lexus_ux_reset_search(instance);
            if(valid && instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        return;
    }

    uint64_t candidate =
        ((instance->window << 1) | (diff_bit ? 1u : 0u)) & ((1ull << LEXUS_UX_HEAD_SEARCH_BITS) - 1u);
    if(candidate == (0x00C480808880ull & ((1ull << LEXUS_UX_HEAD_SEARCH_BITS) - 1u))) {
        instance->decoder.parser_step = LexusUxStepCollecting;
        memset(instance->frame, 0, sizeof(instance->frame));
        instance->frame[0] = 0x00;
        instance->frame[1] = 0xC4;
        instance->frame[2] = 0x80;
        instance->frame[3] = 0x80;
        instance->frame[4] = 0x88;
        instance->frame[5] = 0x80;
        instance->count = 48;
        return;
    }
    instance->window = candidate;
}

uint8_t subghz_protocol_decoder_lexus_ux_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderLexusUx* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < sizeof(instance->frame); i++) hash = (hash * 33u) ^ instance->frame[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_lexus_ux_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderLexusUx* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_lexus_ux_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderLexusUx* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
}

void subghz_protocol_decoder_lexus_ux_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderLexusUx* instance = context;
    uint8_t btn = instance->generic.btn;
    const char* name = (btn == 0x2) ? lexus_ux_names[0] : (btn == 0x4) ? lexus_ux_names[1] : "?";
    furi_string_cat_printf(
        output,
        "%s 256bit\r\n"
        "Id:%08lX Btn:%01X %s\r\n"
        "Code(hi64):%016llX\r\n"
        "Rolling - cipher unknown\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        btn,
        name,
        (unsigned long long)instance->generic.data);
}
