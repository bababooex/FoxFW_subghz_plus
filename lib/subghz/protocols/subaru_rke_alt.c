#include "subaru_rke_alt.h"

#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <stdlib.h>

/*
 * Subaru RKE Alt - EXPERIMENTAL, UNVALIDATED DECODER.
 *
 * Ported on request from the untested scaffolding in auto_rke_protocols.c
 * into the normal decoder shape, decode-only. No cited source, no real
 * capture behind the numbers below - treat every one as a placeholder
 * until confirmed against an actual capture. Distinct from the already-
 * registered "SUBARU" protocol (subaru.c): different, also-unverified
 * frame layout.
 *
 * Bits are sent LSB-first here (bit index 0 first), unlike the other ports
 * in this batch, so they are accumulated by direct bit-position rather than
 * via subghz_protocol_blocks_add_bit()'s shift-left-OR (which assumes
 * MSB-first arrival).
 */

#define SUBARU_ALT_BITS    48u
#define SUBARU_ALT_SYNC_LO 8000u
#define SUBARU_ALT_BIT1_HI 600u
#define SUBARU_ALT_BIT1_LO 200u
#define SUBARU_ALT_BIT0_HI 200u
#define SUBARU_ALT_BIT0_LO 600u
#define SUBARU_ALT_TOL_PCT 25u

#define SUBARU_ALT_BTN_LOCK   0x1u
#define SUBARU_ALT_BTN_UNLOCK 0x2u
#define SUBARU_ALT_BTN_TRUNK  0x4u
#define SUBARU_ALT_BTN_PANIC  0x8u

#define SUBARU_ALT_IN_TOL(x, ref) \
    (DURATION_DIFF((x), (ref)) <= ((ref) * SUBARU_ALT_TOL_PCT / 100u))

typedef enum {
    SubaruAltStepReset = 0,
    SubaruAltStepMarkerHigh,
    SubaruAltStepMarkerLow,
    SubaruAltStepBitHigh,
    SubaruAltStepBitLow,
} SubaruAltDecoderStep;

struct SubGhzProtocolDecoderSubaruRkeAlt {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder; // parser_step only; bits tracked via data/bit_count below
    SubGhzBlockGeneric generic;
    uint64_t data;
    uint8_t bit_count;
    uint8_t pending_bit;
    bool valid;
};

static uint8_t subaru_alt_checksum(uint32_t id, uint8_t ctr, uint8_t btn) {
    uint8_t c = 0;
    for(int i = 0; i < 4; i++) c ^= (id >> (i * 8)) & 0xFFu;
    c ^= ctr ^ (btn & 0xFu);
    return c & 0xFFu;
}

static void subaru_alt_extract_fields(SubGhzProtocolDecoderSubaruRkeAlt* instance) {
    uint64_t word = instance->data;
    uint32_t id = (uint32_t)(word & 0xFFFFFFFFu);
    uint8_t ctr = (uint8_t)((word >> 32) & 0xFFu);
    uint8_t btn = (uint8_t)((word >> 44) & 0xFu);
    uint8_t rx_ck = (uint8_t)((word >> 40) & 0xFu);
    instance->generic.serial = id;
    instance->generic.cnt = ctr;
    instance->generic.btn = btn;
    instance->valid = (rx_ck == (subaru_alt_checksum(id, ctr, btn) & 0xFu));
}

const SubGhzProtocolDecoder subghz_protocol_subaru_rke_alt_decoder = {
    .alloc = subghz_protocol_decoder_subaru_rke_alt_alloc,
    .free = subghz_protocol_decoder_subaru_rke_alt_free,
    .reset = subghz_protocol_decoder_subaru_rke_alt_reset,
    .feed = subghz_protocol_decoder_subaru_rke_alt_feed,
    .get_hash_data = subghz_protocol_decoder_subaru_rke_alt_get_hash_data,
    .serialize = subghz_protocol_decoder_subaru_rke_alt_serialize,
    .deserialize = subghz_protocol_decoder_subaru_rke_alt_deserialize,
    .get_string = subghz_protocol_decoder_subaru_rke_alt_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_subaru_rke_alt_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_subaru_rke_alt = {
    .name = SUBGHZ_PROTOCOL_SUBARU_RKE_ALT_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_subaru_rke_alt_decoder,
    .encoder = &subghz_protocol_subaru_rke_alt_encoder,
};

void* subghz_protocol_decoder_subaru_rke_alt_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderSubaruRkeAlt* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_subaru_rke_alt;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_SUBARU_RKE_ALT_NAME;
    return instance;
}

void subghz_protocol_decoder_subaru_rke_alt_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_subaru_rke_alt_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSubaruRkeAlt* instance = context;
    instance->decoder.parser_step = SubaruAltStepReset;
    instance->data = 0;
    instance->bit_count = 0;
    instance->pending_bit = 0;
    instance->valid = false;
}

void subghz_protocol_decoder_subaru_rke_alt_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderSubaruRkeAlt* instance = context;

    switch(instance->decoder.parser_step) {
    case SubaruAltStepReset:
        // a long idle low is the frame gap/sync in this model
        if(!level && SUBARU_ALT_IN_TOL(duration, SUBARU_ALT_SYNC_LO)) {
            instance->decoder.parser_step = SubaruAltStepMarkerHigh;
        }
        break;

    case SubaruAltStepMarkerHigh:
        if(level && SUBARU_ALT_IN_TOL(duration, SUBARU_ALT_BIT1_HI)) {
            instance->decoder.parser_step = SubaruAltStepMarkerLow;
        } else {
            instance->decoder.parser_step = SubaruAltStepReset;
        }
        break;

    case SubaruAltStepMarkerLow:
        if(!level && SUBARU_ALT_IN_TOL(duration, SUBARU_ALT_BIT1_LO)) {
            instance->data = 0;
            instance->bit_count = 0;
            instance->decoder.parser_step = SubaruAltStepBitHigh;
        } else {
            instance->decoder.parser_step = SubaruAltStepReset;
        }
        break;

    case SubaruAltStepBitHigh:
        if(!level) {
            instance->decoder.parser_step = SubaruAltStepReset;
        } else if(SUBARU_ALT_IN_TOL(duration, SUBARU_ALT_BIT1_HI)) {
            instance->pending_bit = 1;
            instance->decoder.parser_step = SubaruAltStepBitLow;
        } else if(SUBARU_ALT_IN_TOL(duration, SUBARU_ALT_BIT0_HI)) {
            instance->pending_bit = 0;
            instance->decoder.parser_step = SubaruAltStepBitLow;
        } else {
            instance->decoder.parser_step = SubaruAltStepReset;
        }
        break;

    case SubaruAltStepBitLow: {
        if(level) {
            instance->decoder.parser_step = SubaruAltStepReset;
            break;
        }
        bool is_last_bit = (instance->bit_count == SUBARU_ALT_BITS - 1);
        uint32_t expect_lo = instance->pending_bit ? SUBARU_ALT_BIT1_LO : SUBARU_ALT_BIT0_LO;
        bool low_ok = is_last_bit
                          ? (duration >= expect_lo * (100u - SUBARU_ALT_TOL_PCT) / 100u)
                          : SUBARU_ALT_IN_TOL(duration, expect_lo);
        if(!low_ok) {
            instance->decoder.parser_step = SubaruAltStepReset;
            break;
        }

        // LSB-first: bit at position `bit_count`, not a shift-accumulate.
        instance->data |= ((uint64_t)instance->pending_bit << instance->bit_count);
        instance->bit_count++;

        if(instance->bit_count == SUBARU_ALT_BITS) {
            subaru_alt_extract_fields(instance);
            instance->decoder.parser_step = SubaruAltStepReset;
            if(instance->valid && instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
        } else {
            instance->decoder.parser_step = SubaruAltStepBitHigh;
        }
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_subaru_rke_alt_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSubaruRkeAlt* instance = context;
    uint8_t hash = 0;
    for(int i = 0; i < 6; i++) hash = (hash * 33u) ^ ((instance->data >> (i * 8)) & 0xFFu);
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_subaru_rke_alt_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderSubaruRkeAlt* instance = context;
    instance->generic.data = instance->data;
    instance->generic.data_count_bit = SUBARU_ALT_BITS;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_subaru_rke_alt_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderSubaruRkeAlt* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, SUBARU_ALT_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    instance->data = instance->generic.data;
    instance->bit_count = (uint8_t)SUBARU_ALT_BITS;
    subaru_alt_extract_fields(instance);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_subaru_rke_alt_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderSubaruRkeAlt* instance = context;

    FuriString* btn_str = furi_string_alloc();
    uint8_t btn = instance->generic.btn;
    furi_string_cat_str(btn_str, "[");
    if(btn & SUBARU_ALT_BTN_LOCK) furi_string_cat_str(btn_str, "Lock ");
    if(btn & SUBARU_ALT_BTN_UNLOCK) furi_string_cat_str(btn_str, "Unlock ");
    if(btn & SUBARU_ALT_BTN_TRUNK) furi_string_cat_str(btn_str, "Trunk ");
    if(btn & SUBARU_ALT_BTN_PANIC) furi_string_cat_str(btn_str, "Panic ");
    furi_string_cat_str(btn_str, "]");

    furi_string_cat_printf(
        output,
        "%s\r\n"
        "(EXPERIMENTAL / unverified)\r\n"
        "Id:%08lX Cnt:%02lX\r\n"
        "Btn:%01X %s\r\n"
        "Checksum: %s\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        (unsigned long)instance->generic.cnt,
        instance->generic.btn,
        furi_string_get_cstr(btn_str),
        instance->valid ? "OK" : "FAIL");

    furi_string_free(btn_str);
}
