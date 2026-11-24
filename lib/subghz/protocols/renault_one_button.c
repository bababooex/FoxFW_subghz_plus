#include "renault_one_button.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <string.h>
#include <stdlib.h>

/*
 * Renault 1-Button: an old one-button Renault remote (the board is dated
 * 1999), DECODE ONLY. KAT's own decoder (github.com/KaraZajac/KAT,
 * src/protocols/renault_one_button.rs), worked out from one real recording
 * (110 presses); no other reference reads this signal, and it's none of
 * our other renault_*.c protocols.
 *
 * Air format: Manchester (OOK), Te=473.5us. A press repeats the frame 2-4
 * times, each copy separated by a real gap (14ms pause + 8.7ms carrier) -
 * so each copy is its own self-contained burst, not one continuous stream.
 *
 * Rewritten (2026-11-22) from a forward head-search (which never actually
 * validated against a real capture - see below) to KAT's own whole-burst
 * approach, confirmed against a real user capture (350 of 443 bursts
 * decoded correctly; the original forward search decoded 0 of 346):
 *   1. Collect every half-bit LEVEL of a burst (a pulse is 1 or 2 half
 *      bits; anything else ends the burst) into a flat buffer, prefixed by
 *      one extra half-bit for whatever level came right before the burst
 *      started (or, after a reset, the opposite of the burst's own first
 *      level) - a frame can start or end mid-half-bit, and that boundary
 *      half belongs to it.
 *   2. Once the burst ends, pair up the halves two at a time from the
 *      START OF THE BUFFER PLUS 0 OR 1 (the two ways the pairing can land,
 *      since we don't know in advance how many boundary halves preceded
 *      the real frame) - a pair that differs is a bit (its value = the
 *      pair's first half); a pair that doesn't is "not a bit" and fails
 *      anything that needs it.
 *   3. Anchor the 72-bit frame from the END of the burst, not the start:
 *      the burst's very last bit must be the closing "1" (trailing
 *      carrier + the pause behind it), and at least 2 zero bits must
 *      immediately precede the 72-bit frame taken right before that.
 *   4. Validate: the top 4 bits of the first 64 bits must be 0b0011 (the
 *      "11" marker, with 2 zero bits ahead of it folded in), XORing the
 *      check byte into all 8 data bytes must give 0, and the 24-bit ID
 *      (bits 36..60) must not be 0.
 * Trying BOTH phases at a real head-search from the front (the original
 * approach) can land one bit off depending on exactly how many preamble
 * bits came before the frame - which is exactly the failure mode a real
 * capture exposed.
 *
 * Frame (72 bits = 9 bytes, counting the 2 leading zero bits folded into
 * the top of the first byte): 2 zero bits, "11", a 24-bit ID (constant
 * per remote), 36 bits that change with every press (no bit of it was
 * ever seen to follow from the press before, across 110 presses - not a
 * simple counter), and an 8-bit check byte: XORing all 9 bytes together
 * gives 0. `data` (64 bits, for storage) is the first 8 bytes; the 9th is
 * kept only to redisplay/recheck, not separately exposed.
 */

#define RENAULT_ONE_BUTTON_TE 473u
#define RENAULT_ONE_BUTTON_TOL 120u
#define RENAULT_ONE_BUTTON_MIN_ZEROS 2u
#define RENAULT_ONE_BUTTON_FRAME_BITS 72u /* 64 (data) + 8 (check) */
#define RENAULT_ONE_BUTTON_HALVES_CAP 512u

#define RENAULT_ONE_BUTTON_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < RENAULT_ONE_BUTTON_TOL)

struct SubGhzProtocolDecoderRenaultOneButton {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    bool halves[RENAULT_ONE_BUTTON_HALVES_CAP];
    uint16_t halves_count;
    bool have_before;
    bool before;
    /* Scratch space for renault_one_button_try_phase() - kept here (not a
     * stack-local array) since feed() runs on a worker thread/ISR with a
     * limited stack. */
    int8_t bits_scratch[RENAULT_ONE_BUTTON_HALVES_CAP / 2u];
};

const SubGhzProtocolDecoder subghz_protocol_renault_one_button_decoder = {
    .alloc = subghz_protocol_decoder_renault_one_button_alloc,
    .free = subghz_protocol_decoder_renault_one_button_free,
    .reset = subghz_protocol_decoder_renault_one_button_reset,
    .feed = subghz_protocol_decoder_renault_one_button_feed,
    .get_hash_data = subghz_protocol_decoder_renault_one_button_get_hash_data,
    .serialize = subghz_protocol_decoder_renault_one_button_serialize,
    .deserialize = subghz_protocol_decoder_renault_one_button_deserialize,
    .get_string = subghz_protocol_decoder_renault_one_button_get_string,
};

/* Decode-only - the rolling part can't be continued (no known cipher). */
const SubGhzProtocolEncoder subghz_protocol_renault_one_button_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_renault_one_button = {
    .name = SUBGHZ_PROTOCOL_RENAULT_ONE_BUTTON_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_renault_one_button_decoder,
    .encoder = &subghz_protocol_renault_one_button_encoder,
};

static bool renault_one_button_fields(SubGhzBlockGeneric* generic, uint64_t data, uint8_t check) {
    uint8_t x = check;
    for(int i = 0; i < 8; i++) x ^= (uint8_t)((data >> (8 * i)) & 0xFFu);
    if(x != 0) return false;
    if(((data >> 60) & 0xFu) != 0x3u) return false; /* the 2 leading zeros + "11" */
    uint32_t serial = (uint32_t)((data >> 36) & 0xFFFFFFu);
    if(serial == 0) return false;

    generic->data = data;
    generic->data_count_bit = 64;
    generic->serial = serial;
    generic->seed = check; /* the check byte, for display */
    return true;
}

void* subghz_protocol_decoder_renault_one_button_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderRenaultOneButton* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_renault_one_button;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_RENAULT_ONE_BUTTON_NAME;
    return instance;
}

void subghz_protocol_decoder_renault_one_button_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_renault_one_button_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderRenaultOneButton* instance = context;
    instance->halves_count = 0;
    instance->have_before = false;
}

static void
    renault_one_button_push_half(SubGhzProtocolDecoderRenaultOneButton* instance, bool level) {
    if(instance->halves_count >= RENAULT_ONE_BUTTON_HALVES_CAP) {
        /* Keep the newest half of the buffer (same "a longer burst loses its
         * oldest half" rule KAT's own BurstReader uses) - a real frame never
         * gets anywhere near this cap, so this only guards pathological input. */
        uint16_t keep = RENAULT_ONE_BUTTON_HALVES_CAP / 2u;
        uint16_t drop = (uint16_t)(instance->halves_count - keep);
        memmove(instance->halves, instance->halves + drop, keep * sizeof(bool));
        instance->halves_count = keep;
    }
    instance->halves[instance->halves_count++] = level;
}

/* Tries pairing the collected halves from `phase` (0 or 1) halves into the
 * burst: a differing pair is a bit (its value = the pair's first half), a
 * matching pair is "not a bit" (fails anything that needs it). Anchors the
 * 72-bit frame from the END (the burst's last bit must be the closing "1",
 * and >=2 zero bits must immediately precede the frame). Returns the 64-bit
 * data word and the 8-bit check byte on success. */
static bool renault_one_button_try_phase(
    SubGhzProtocolDecoderRenaultOneButton* instance,
    uint8_t phase,
    uint64_t* data_out,
    uint8_t* check_out) {
    const bool* halves = instance->halves;
    uint16_t halves_count = instance->halves_count;
    if(halves_count <= phase) return false;
    uint16_t avail = (uint16_t)(halves_count - phase);
    uint16_t nbits = avail / 2u;
    uint16_t needed = RENAULT_ONE_BUTTON_FRAME_BITS + 1u; /* +1 for the closing "1" bit */
    if(nbits < needed) return false;

    /* bit value: 1/0, or -1 for "not a bit" (the pair's two halves matched). */
    int8_t* bits = instance->bits_scratch;
    for(uint16_t i = 0; i < nbits; i++) {
        bool a = halves[phase + i * 2u];
        bool b = halves[phase + i * 2u + 1u];
        bits[i] = (a != b) ? (int8_t)(a ? 1 : 0) : (int8_t)-1;
    }

    int32_t start = (int32_t)nbits - (int32_t)needed;
    if(start < 0) return false;
    if(bits[nbits - 1] != 1) return false; /* the closing bit must be "1" */

    uint16_t zero_run = 0;
    for(int32_t i = start - 1; i >= 0; i--) {
        if(bits[i] == 0) {
            zero_run++;
        } else {
            break;
        }
    }
    if(zero_run < RENAULT_ONE_BUTTON_MIN_ZEROS) return false;

    uint64_t data = 0;
    for(uint8_t i = 0; i < 64; i++) {
        int8_t b = bits[start + i];
        if(b < 0) return false;
        data = (data << 1) | (uint64_t)b;
    }
    uint8_t check = 0;
    for(uint8_t i = 0; i < 8; i++) {
        int8_t b = bits[start + 64 + i];
        if(b < 0) return false;
        check = (uint8_t)((check << 1) | (uint8_t)b);
    }

    *data_out = data;
    *check_out = check;
    return true;
}

static void
    renault_one_button_process_burst(SubGhzProtocolDecoderRenaultOneButton* instance) {
    for(uint8_t phase = 0; phase < 2; phase++) {
        uint64_t data;
        uint8_t check;
        if(renault_one_button_try_phase(instance, phase, &data, &check)) {
            if(renault_one_button_fields(&instance->generic, data, check)) {
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
                return;
            }
        }
    }
}

void
    subghz_protocol_decoder_renault_one_button_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderRenaultOneButton* instance = context;

    uint8_t units;
    if(RENAULT_ONE_BUTTON_IN_TOL(duration, RENAULT_ONE_BUTTON_TE)) {
        units = 1;
    } else if(RENAULT_ONE_BUTTON_IN_TOL(duration, 2u * RENAULT_ONE_BUTTON_TE)) {
        units = 2;
    } else {
        units = 0;
    }

    if(units == 0) {
        /* Anything that isn't a clean 1- or 2-half-bit pulse ends the burst -
         * a real inter-copy gap (14ms) falls here same as any glitch. */
        if(instance->halves_count > 0) {
            renault_one_button_push_half(instance, level);
            renault_one_button_process_burst(instance);
        }
        instance->halves_count = 0;
        instance->have_before = true;
        instance->before = level;
        return;
    }

    if(instance->halves_count == 0) {
        bool lead = instance->have_before ? instance->before : !level;
        renault_one_button_push_half(instance, lead);
    }
    for(uint8_t i = 0; i < units; i++) {
        renault_one_button_push_half(instance, level);
    }
}

uint8_t subghz_protocol_decoder_renault_one_button_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderRenaultOneButton* instance = context;
    uint64_t v = instance->generic.data;
    uint8_t hash = instance->generic.seed;
    for(int i = 0; i < 8; i++) hash ^= (uint8_t)((v >> (8 * i)) & 0xFFu);
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_renault_one_button_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderRenaultOneButton* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_renault_one_button_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderRenaultOneButton* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
}

void subghz_protocol_decoder_renault_one_button_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderRenaultOneButton* instance = context;
    furi_string_cat_printf(
        output,
        "%s 72bit\r\n"
        "Id:%06lX\r\n"
        "Key:%016llX Chk:%02lX\r\n"
        "Rolling - cipher unknown\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->generic.seed);
}
