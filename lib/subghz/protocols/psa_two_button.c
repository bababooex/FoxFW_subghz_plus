#include "psa_two_button.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#include <furi.h>
#include <string.h>
#include <stdlib.h>

/*
 * PSA 2-Button: a two-button Valeo flip key for Citroen/Peugeot, DECODE
 * ONLY. KAT's own decoder (github.com/KaraZajac/KAT, src/protocols/
 * psa_two_button.rs), worked out from two real recordings of one key (50
 * presses each); no other reference reads this signal. NOT the same frame
 * as our psa.c/psa2.c (80-bit TEA/XOR).
 *
 * Air format: Manchester (OOK), Te=207us. A press: ~78ms of wake-up
 * carrier, a 6.9ms pause, then the frame (a lone leading carrier half-bit,
 * 76 Manchester bits, then a long closing carrier pulse of 2.0-3.3ms that
 * marks the end - much longer than any in-frame half-bit, so it's an
 * unambiguous end-of-frame marker). This decoder starts collecting after
 * any gap bigger than a normal in-frame one and reports when the closing
 * pulse arrives with exactly 76 bits collected; the lone leading half-bit
 * is absorbed by the Manchester decoder's own post-reset synchronization,
 * the same way every other Manchester protocol in this tree handles its
 * first edge after a reset.
 *
 * Frame (76 bits): a fixed byte (0xCE in the one key recorded), a 4-bit
 * button nibble (0xC Unlock, 0x3 Lock), then a 64-bit code that changes
 * with every press. KAT could not find a checksum or continue the code
 * (it's not a strong cipher's output - its bytes mix a counter, a check
 * byte and some constant bytes - but which byte is which isn't settled);
 * it's shown here as an opaque hex value.
 */

#define PSA_TWO_BUTTON_TE 207u
#define PSA_TWO_BUTTON_TOL 90u
#define PSA_TWO_BUTTON_BITS 76u
#define PSA_TWO_BUTTON_BYTES 10u /* 80 bits allocated, 76 used */
#define PSA_TWO_BUTTON_RESET_GAP 1000u
#define PSA_TWO_BUTTON_END_MIN 2000u
#define PSA_TWO_BUTTON_END_MAX 3300u

#define PSA_TWO_BUTTON_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < PSA_TWO_BUTTON_TOL)

struct SubGhzProtocolDecoderPsaTwoButton {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint8_t frame[PSA_TWO_BUTTON_BYTES];
    uint8_t count;
};

static const char* const psa_two_button_names[2] = {"Unlock", "Lock"};

const SubGhzProtocolDecoder subghz_protocol_psa_two_button_decoder = {
    .alloc = subghz_protocol_decoder_psa_two_button_alloc,
    .free = subghz_protocol_decoder_psa_two_button_free,
    .reset = subghz_protocol_decoder_psa_two_button_reset,
    .feed = subghz_protocol_decoder_psa_two_button_feed,
    .get_hash_data = subghz_protocol_decoder_psa_two_button_get_hash_data,
    .serialize = subghz_protocol_decoder_psa_two_button_serialize,
    .deserialize = subghz_protocol_decoder_psa_two_button_deserialize,
    .get_string = subghz_protocol_decoder_psa_two_button_get_string,
};

/* Decode-only - the cipher is unknown. */
const SubGhzProtocolEncoder subghz_protocol_psa_two_button_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_psa_two_button = {
    .name = SUBGHZ_PROTOCOL_PSA_TWO_BUTTON_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_psa_two_button_decoder,
    .encoder = &subghz_protocol_psa_two_button_encoder,
};

/* Reads `len` bits (len <= 64) starting at bit `start`, MSB-first, out of a
 * byte array packed the same way (bit 0 = the MSB of buf[0]). */
static uint64_t psa_two_button_read_bits(const uint8_t* buf, size_t start, size_t len) {
    uint64_t v = 0;
    for(size_t i = 0; i < len; i++) {
        size_t pos = start + i;
        uint8_t bit = (buf[pos / 8] >> (7 - (pos % 8))) & 1u;
        v = (v << 1) | bit;
    }
    return v;
}

void* subghz_protocol_decoder_psa_two_button_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderPsaTwoButton* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_psa_two_button;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_PSA_TWO_BUTTON_NAME;
    return instance;
}

void subghz_protocol_decoder_psa_two_button_free(void* context) {
    furi_check(context);
    free(context);
}

static void psa_two_button_reset_search(SubGhzProtocolDecoderPsaTwoButton* instance) {
    memset(instance->frame, 0, sizeof(instance->frame));
    instance->count = 0;
    instance->manchester_state = ManchesterStateMid1;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_psa_two_button_reset(void* context) {
    furi_check(context);
    psa_two_button_reset_search(context);
}

void subghz_protocol_decoder_psa_two_button_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderPsaTwoButton* instance = context;

    if(level && duration >= PSA_TWO_BUTTON_END_MIN && duration <= PSA_TWO_BUTTON_END_MAX) {
        if(instance->count == PSA_TWO_BUTTON_BITS) {
            uint8_t fixed_byte = (uint8_t)psa_two_button_read_bits(instance->frame, 0, 8);
            uint8_t button = (uint8_t)psa_two_button_read_bits(instance->frame, 8, 4);
            uint64_t code = psa_two_button_read_bits(instance->frame, 12, 64);
            UNUSED(fixed_byte);
            instance->generic.btn = button;
            instance->generic.data = code;
            instance->generic.data_count_bit = 64;
            psa_two_button_reset_search(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
            return;
        }
        psa_two_button_reset_search(instance);
        return;
    }

    if(duration > PSA_TWO_BUTTON_RESET_GAP) {
        psa_two_button_reset_search(instance);
        return;
    }

    ManchesterEvent event;
    if(PSA_TWO_BUTTON_IN_TOL(duration, PSA_TWO_BUTTON_TE)) {
        event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    } else if(PSA_TWO_BUTTON_IN_TOL(duration, 2u * PSA_TWO_BUTTON_TE)) {
        event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    } else {
        psa_two_button_reset_search(instance);
        return;
    }

    bool bit;
    if(!manchester_advance(
           instance->manchester_state, event, &instance->manchester_state, &bit)) {
        return;
    }

    if(instance->count < PSA_TWO_BUTTON_BITS) {
        if(bit) instance->frame[instance->count / 8] |= (uint8_t)(0x80 >> (instance->count % 8));
        instance->count++;
    } else {
        /* More than 76 bits before the closing pulse: not a valid frame. */
        psa_two_button_reset_search(instance);
    }
}

uint8_t subghz_protocol_decoder_psa_two_button_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderPsaTwoButton* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < sizeof(instance->frame); i++) hash = (hash * 33u) ^ instance->frame[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_psa_two_button_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderPsaTwoButton* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_psa_two_button_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderPsaTwoButton* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
}

void subghz_protocol_decoder_psa_two_button_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderPsaTwoButton* instance = context;
    uint8_t btn = instance->generic.btn;
    const char* name = (btn == 0xC) ? psa_two_button_names[0] :
                       (btn == 0x3) ? psa_two_button_names[1] :
                                      "?";
    furi_string_cat_printf(
        output,
        "%s 76bit\r\n"
        "Btn:%01X %s\r\n"
        "Code:%016llX\r\n"
        "Rolling - cipher unknown\r\n",
        instance->generic.protocol_name,
        btn,
        name,
        (unsigned long long)instance->generic.data);
}
