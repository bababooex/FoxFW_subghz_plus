#include "volvo_s40.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"

#include <furi.h>
#include <stdlib.h>

/*
 * Volvo S40 (1999 remote), DECODE ONLY. Ported from the KAT project's own
 * decoder (github.com/KaraZajac/KAT, src/protocols/volvo_s40.rs) - worked
 * out from a single real recording (two presses of one button) in KAT's
 * own capture library; no other reference reads this signal.
 *
 * The air format and framing rule are exactly our chrysler.c family's
 * (Chrysler V0 / the Chrysler PT Cruiser fob), just with a 64-bit frame
 * instead of 80 bits. Worked out independently by KAT, not ported FROM our
 * chrysler protocols - included here as its own file since the bit count
 * differs and our chrysler.c doesn't use this "sum byte + XOR" framing
 * rule at all (see the KAT doc for how that rule was confirmed: the odd-
 * sum frame reads back the same six plaintext bytes as the even-sum one,
 * which happens by chance only 1 in 2^48).
 *
 * Air format (AM, 433.92 MHz): pulse-width coding, 4ms/bit - carrier 300us
 * + pause 3.7ms = bit 0, carrier 600us + pause 3.4ms = bit 1. A press: 16+
 * preamble pairs (carrier 300us, pause 3.7ms), a short carrier, a
 * 12-20ms gap, then the 64-bit frame, a short carrier, a gap over 8ms
 * (closes the frame) - sent twice (the second copy has no preamble).
 *
 * Frame rule (each byte sent LSB-first): the remote holds 7 bytes, n (a
 * press counter's low byte, decrementing) and p1..p6. The frame is: a sum
 * byte (n+p1+...+p6 mod 256), then n, then p1^n .. p6^n. When the sum is
 * odd, the 56 bits behind the sum byte go out bit-reversed (last bit
 * first) instead. Nothing is encrypted - only the counter changes between
 * presses. Which byte (if any) is the button isn't known (only one button
 * was ever recorded), so no button is reported.
 *
 * No encoder (KAT doesn't offer one either: with only one button recorded,
 * it can't safely send any specific one - see the file's own "Transmit"
 * notes).
 */

#define VOLVO_S40_TE_SHORT 300u /* carrier for bit 0, and every preamble pulse */
#define VOLVO_S40_TE_ONE 600u /* carrier for bit 1 */
#define VOLVO_S40_TE_DELTA 150u
#define VOLVO_S40_PAUSE_MIN 3000u
#define VOLVO_S40_PAUSE_MAX 4100u
#define VOLVO_S40_START_GAP_MIN 12000u
#define VOLVO_S40_START_GAP_MAX 20000u
#define VOLVO_S40_END_GAP_MIN 8000u
#define VOLVO_S40_PREAMBLE_MIN 16u
#define VOLVO_S40_BITS 64u

static inline bool volvo_s40_is_zero(uint32_t d) {
    return d >= VOLVO_S40_TE_SHORT - VOLVO_S40_TE_DELTA && d <= VOLVO_S40_TE_SHORT + VOLVO_S40_TE_DELTA;
}
static inline bool volvo_s40_is_one(uint32_t d) {
    return d > VOLVO_S40_TE_SHORT + VOLVO_S40_TE_DELTA && d <= VOLVO_S40_TE_ONE + VOLVO_S40_TE_DELTA;
}
static inline bool volvo_s40_is_pause(uint32_t d) {
    return d >= VOLVO_S40_PAUSE_MIN && d <= VOLVO_S40_PAUSE_MAX;
}

typedef enum {
    VolvoS40StepIdle = 0,
    VolvoS40StepPreamblePause,
    VolvoS40StepPreambleCarrier,
    VolvoS40StepBitCarrier,
    VolvoS40StepBitPauseZero,
    VolvoS40StepBitPauseOne,
} VolvoS40DecoderStep;

struct SubGhzProtocolDecoderVolvoS40 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    size_t pairs;
};

static uint8_t volvo_s40_reverse_bits8(uint8_t v) {
    uint8_t r = 0;
    for(int i = 0; i < 8; i++) {
        r = (uint8_t)((r << 1) | (v & 1u));
        v >>= 1;
    }
    return r;
}

/* The frame's rule: the sum byte and the 7 bytes behind it (n, then
 * p1^n..p6^n), possibly sent in reverse byte order when the sum is odd -
 * see the file header. Returns false when the sum doesn't hold. */
static bool volvo_s40_unmask(uint64_t frame, uint8_t* n_out, uint8_t plain[6]) {
    uint8_t raw[8];
    for(int i = 0; i < 8; i++) raw[i] = (uint8_t)(frame >> (8 * (7 - i)));

    uint8_t sum = volvo_s40_reverse_bits8(raw[0]);
    uint8_t bytes[7];
    if((sum & 1u) == 0) {
        for(int i = 0; i < 7; i++) bytes[i] = volvo_s40_reverse_bits8(raw[1 + i]);
    } else {
        for(int i = 0; i < 7; i++) bytes[i] = raw[7 - i];
    }

    uint8_t n = bytes[0];
    uint8_t total = n;
    for(int i = 0; i < 6; i++) {
        plain[i] = (uint8_t)(bytes[1 + i] ^ n);
        total = (uint8_t)(total + plain[i]);
    }
    if(total != sum) return false;
    *n_out = n;
    return true;
}

static bool volvo_s40_fields(SubGhzBlockGeneric* generic, uint64_t frame) {
    uint8_t n, plain[6];
    if(!volvo_s40_unmask(frame, &n, plain)) return false;
    uint32_t id = ((uint32_t)plain[0] << 24) | ((uint32_t)plain[1] << 16) |
                  ((uint32_t)plain[2] << 8) | (uint32_t)plain[3];
    if(id == 0 || id == 0xFFFFFFFFu) return false;

    generic->data = frame;
    generic->data_count_bit = VOLVO_S40_BITS;
    generic->serial = id;
    generic->cnt = n;
    generic->seed = ((uint32_t)plain[4] << 8) | plain[5]; /* p5, p6 - unknown meaning */
    return true;
}

const SubGhzProtocolDecoder subghz_protocol_volvo_s40_decoder = {
    .alloc = subghz_protocol_decoder_volvo_s40_alloc,
    .free = subghz_protocol_decoder_volvo_s40_free,
    .reset = subghz_protocol_decoder_volvo_s40_reset,
    .feed = subghz_protocol_decoder_volvo_s40_feed,
    .get_hash_data = subghz_protocol_decoder_volvo_s40_get_hash_data,
    .serialize = subghz_protocol_decoder_volvo_s40_serialize,
    .deserialize = subghz_protocol_decoder_volvo_s40_deserialize,
    .get_string = subghz_protocol_decoder_volvo_s40_get_string,
};

/* Decode-only - see the file header's "Transmit" note. */
const SubGhzProtocolEncoder subghz_protocol_volvo_s40_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_volvo_s40 = {
    .name = SUBGHZ_PROTOCOL_VOLVO_S40_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_volvo_s40_decoder,
    .encoder = &subghz_protocol_volvo_s40_encoder,
};

void* subghz_protocol_decoder_volvo_s40_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderVolvoS40* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_volvo_s40;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_VOLVO_S40_NAME;
    return instance;
}

void subghz_protocol_decoder_volvo_s40_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_volvo_s40_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoS40* instance = context;
    instance->decoder.parser_step = VolvoS40StepIdle;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->pairs = 0;
}

/* A pulse that fits nowhere: start over, taking a short carrier itself as
 * the first preamble pulse of a fresh attempt (self-resyncing, as the
 * reference does). */
static void volvo_s40_restart(SubGhzProtocolDecoderVolvoS40* instance, bool level, uint32_t duration) {
    instance->pairs = 0;
    instance->decoder.parser_step =
        (level && volvo_s40_is_zero(duration)) ? VolvoS40StepPreamblePause : VolvoS40StepIdle;
}

void subghz_protocol_decoder_volvo_s40_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoS40* instance = context;

    switch(instance->decoder.parser_step) {
    case VolvoS40StepIdle:
        volvo_s40_restart(instance, level, duration);
        break;

    case VolvoS40StepPreamblePause:
        if(!level && volvo_s40_is_pause(duration)) {
            instance->pairs++;
            instance->decoder.parser_step = VolvoS40StepPreambleCarrier;
        } else if(
            !level && instance->pairs >= VOLVO_S40_PREAMBLE_MIN && duration >= VOLVO_S40_START_GAP_MIN &&
            duration <= VOLVO_S40_START_GAP_MAX) {
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = VolvoS40StepBitCarrier;
        } else {
            volvo_s40_restart(instance, level, duration);
        }
        break;

    case VolvoS40StepPreambleCarrier:
        if(level && volvo_s40_is_zero(duration)) {
            instance->decoder.parser_step = VolvoS40StepPreamblePause;
        } else {
            volvo_s40_restart(instance, level, duration);
        }
        break;

    case VolvoS40StepBitCarrier:
        if(level && volvo_s40_is_zero(duration)) {
            instance->decoder.parser_step = VolvoS40StepBitPauseZero;
        } else if(level && volvo_s40_is_one(duration)) {
            instance->decoder.parser_step = VolvoS40StepBitPauseOne;
        } else {
            volvo_s40_restart(instance, level, duration);
        }
        break;

    case VolvoS40StepBitPauseZero:
    case VolvoS40StepBitPauseOne: {
        bool one = (instance->decoder.parser_step == VolvoS40StepBitPauseOne);
        if(!level && volvo_s40_is_pause(duration) &&
           instance->decoder.decode_count_bit < VOLVO_S40_BITS) {
            subghz_protocol_blocks_add_bit(&instance->decoder, one ? 1 : 0);
            instance->decoder.parser_step = VolvoS40StepBitCarrier;
        } else if(
            !level && duration > VOLVO_S40_END_GAP_MIN &&
            instance->decoder.decode_count_bit == VOLVO_S40_BITS && !one) {
            /* The short carrier and the pause that close the frame. */
            instance->decoder.parser_step = VolvoS40StepIdle;
            if(volvo_s40_fields(&instance->generic, instance->decoder.decode_data)) {
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
        } else {
            /* A 65th bit, a 1's carrier before the gap, or a frame cut short. */
            volvo_s40_restart(instance, level, duration);
        }
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_volvo_s40_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoS40* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_volvo_s40_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoS40* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_volvo_s40_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoS40* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, VOLVO_S40_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    if(!volvo_s40_fields(&instance->generic, instance->generic.data))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_volvo_s40_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoS40* instance = context;
    furi_string_cat_printf(
        output,
        "%s 64bit\r\n"
        "Key:%016llX\r\n"
        "Id:%08lX Cnt:%02lX\r\n"
        "p5p6:%04lX (unknown, no button ID)\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->generic.serial,
        (unsigned long)instance->generic.cnt,
        (unsigned long)instance->generic.seed);
}
