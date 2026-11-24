#include "hkr.h"

#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <stdlib.h>

/*
 * HKR (Hyundai/Kia-shaped remote) - EXPERIMENTAL, UNVALIDATED DECODER.
 *
 * Ported on request from the untested scaffolding in auto_rke_protocols.c
 * into the normal decoder shape, decode-only. The timing constants and
 * checksum below have NO cited source or real capture behind them - treat
 * every number here as a placeholder until someone confirms it against an
 * actual capture (unlike e.g. vag_v1.c / proteco.c, which ARE grounded in
 * a cited source and real captures - this file is not in their company).
 */

#define HKR_BITS     64u
#define HKR_SYNC_HI  312u
#define HKR_SYNC_LO  10400u
#define HKR_BIT1_HI  728u
#define HKR_BIT1_LO  312u
#define HKR_BIT0_HI  312u
#define HKR_BIT0_LO  728u
#define HKR_TOL_PCT  25u

#define HKR_BTN_LOCK   0x0100u
#define HKR_BTN_UNLOCK 0x0200u
#define HKR_BTN_TRUNK  0x0400u
#define HKR_BTN_PANIC  0x0800u

#define HKR_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) <= ((ref) * HKR_TOL_PCT / 100u))

typedef enum {
    HkrStepReset = 0,
    HkrStepSyncLow,
    HkrStepBitHigh,
    HkrStepBitLow,
} HkrDecoderStep;

struct SubGhzProtocolDecoderHkr {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint8_t pending_bit;
    bool valid;
};

static uint16_t hkr_checksum(uint32_t serial, uint16_t btn) {
    uint16_t c = (uint16_t)(serial ^ (serial >> 16));
    c ^= btn;
    return (uint16_t)(~c);
}

static void hkr_extract_fields(SubGhzProtocolDecoderHkr* instance) {
    uint64_t data = instance->decoder.decode_data;
    uint32_t serial = (uint32_t)(data >> 32);
    uint16_t btn_mask = (uint16_t)((data >> 16) & 0xFFFFu);
    uint16_t ck = (uint16_t)(data & 0xFFFFu);
    instance->generic.serial = serial;
    instance->generic.cnt = btn_mask;
    instance->generic.btn = (uint8_t)(btn_mask >> 8);
    instance->valid = (ck == hkr_checksum(serial, btn_mask));
}

const SubGhzProtocolDecoder subghz_protocol_hkr_decoder = {
    .alloc = subghz_protocol_decoder_hkr_alloc,
    .free = subghz_protocol_decoder_hkr_free,
    .reset = subghz_protocol_decoder_hkr_reset,
    .feed = subghz_protocol_decoder_hkr_feed,
    .get_hash_data = subghz_protocol_decoder_hkr_get_hash_data,
    .serialize = subghz_protocol_decoder_hkr_serialize,
    .deserialize = subghz_protocol_decoder_hkr_deserialize,
    .get_string = subghz_protocol_decoder_hkr_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_hkr_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_hkr = {
    .name = SUBGHZ_PROTOCOL_HKR_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_hkr_decoder,
    .encoder = &subghz_protocol_hkr_encoder,
};

void* subghz_protocol_decoder_hkr_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHkr* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_hkr;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_HKR_NAME;
    return instance;
}

void subghz_protocol_decoder_hkr_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_hkr_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHkr* instance = context;
    instance->decoder.parser_step = HkrStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->pending_bit = 0;
    instance->valid = false;
}

void subghz_protocol_decoder_hkr_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderHkr* instance = context;

    switch(instance->decoder.parser_step) {
    case HkrStepReset:
        if(level && HKR_IN_TOL(duration, HKR_SYNC_HI)) {
            instance->decoder.parser_step = HkrStepSyncLow;
        }
        break;

    case HkrStepSyncLow:
        if(!level && HKR_IN_TOL(duration, HKR_SYNC_LO)) {
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = HkrStepBitHigh;
        } else {
            instance->decoder.parser_step = HkrStepReset;
        }
        break;

    case HkrStepBitHigh:
        if(!level) {
            instance->decoder.parser_step = HkrStepReset;
        } else if(HKR_IN_TOL(duration, HKR_BIT1_HI)) {
            instance->pending_bit = 1;
            instance->decoder.parser_step = HkrStepBitLow;
        } else if(HKR_IN_TOL(duration, HKR_BIT0_HI)) {
            instance->pending_bit = 0;
            instance->decoder.parser_step = HkrStepBitLow;
        } else {
            instance->decoder.parser_step = HkrStepReset;
        }
        break;

    case HkrStepBitLow: {
        if(level) {
            instance->decoder.parser_step = HkrStepReset;
            break;
        }
        bool is_last_bit = (instance->decoder.decode_count_bit == HKR_BITS - 1);
        uint32_t expect_lo = instance->pending_bit ? HKR_BIT1_LO : HKR_BIT0_LO;
        bool low_ok = is_last_bit ? (duration >= expect_lo * (100u - HKR_TOL_PCT) / 100u)
                                   : HKR_IN_TOL(duration, expect_lo);
        if(!low_ok) {
            instance->decoder.parser_step = HkrStepReset;
            break;
        }

        subghz_protocol_blocks_add_bit(&instance->decoder, instance->pending_bit);

        if(instance->decoder.decode_count_bit == HKR_BITS) {
            hkr_extract_fields(instance);
            instance->decoder.parser_step = HkrStepReset;
            if(instance->valid && instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
        } else {
            instance->decoder.parser_step = HkrStepBitHigh;
        }
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_hkr_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHkr* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_hkr_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderHkr* instance = context;
    instance->generic.data = instance->decoder.decode_data;
    instance->generic.data_count_bit = HKR_BITS;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_hkr_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderHkr* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, HKR_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = (uint8_t)HKR_BITS;
    hkr_extract_fields(instance);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_hkr_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderHkr* instance = context;
    uint16_t btn_mask = (uint16_t)instance->generic.cnt;

    FuriString* btn_str = furi_string_alloc();
    furi_string_cat_str(btn_str, "[");
    if(btn_mask & HKR_BTN_LOCK) furi_string_cat_str(btn_str, "Lock ");
    if(btn_mask & HKR_BTN_UNLOCK) furi_string_cat_str(btn_str, "Unlock ");
    if(btn_mask & HKR_BTN_TRUNK) furi_string_cat_str(btn_str, "Trunk ");
    if(btn_mask & HKR_BTN_PANIC) furi_string_cat_str(btn_str, "Panic ");
    furi_string_cat_str(btn_str, "]");

    furi_string_cat_printf(
        output,
        "%s\r\n"
        "(EXPERIMENTAL / unverified)\r\n"
        "Key:%016llX\r\n"
        "Serial:%08lX\r\n"
        "BtnMask:%04X %s\r\n"
        "Checksum: %s\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->decoder.decode_data,
        (unsigned long)instance->generic.serial,
        btn_mask,
        furi_string_get_cstr(btn_str),
        instance->valid ? "OK" : "FAIL");

    furi_string_free(btn_str);
}
