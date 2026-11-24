#include "mazda_v3.h"

#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <stdlib.h>
#include <string.h>

/*
 * Mazda V3 - EXPERIMENTAL, UNVALIDATED DECODER.
 *
 * Ported on request from the untested scaffolding in auto_rke_protocols.c
 * into the normal decoder shape, decode-only. No cited source, no real
 * capture behind the numbers below - treat every one as a placeholder
 * until confirmed against an actual capture.
 *
 * Frame is 9 bytes (72 bits) - too wide for SubGhzBlockDecoder's 64-bit
 * decode_data, so bits are packed directly into a local byte array instead
 * of going through subghz_protocol_blocks_add_bit().
 */

#define MAZDA_V3_BITS    72u
#define MAZDA_V3_BYTES   9u
#define MAZDA_V3_SYNC_HI 450u
#define MAZDA_V3_SYNC_LO 14400u
#define MAZDA_V3_HI      450u /* constant mark length for both bit values */
#define MAZDA_V3_BIT1_LO 1350u
#define MAZDA_V3_BIT0_LO 450u
#define MAZDA_V3_TOL_PCT 25u

#define MAZDA_V3_BTN_LOCK   0x1u
#define MAZDA_V3_BTN_UNLOCK 0x2u
#define MAZDA_V3_BTN_TRUNK  0x4u

#define MAZDA_V3_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) <= ((ref) * MAZDA_V3_TOL_PCT / 100u))

typedef enum {
    MazdaV3StepReset = 0,
    MazdaV3StepSyncLow,
    MazdaV3StepBitHigh,
    MazdaV3StepBitLow,
} MazdaV3DecoderStep;

struct SubGhzProtocolDecoderMazdaV3 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder; // only parser_step / decode_count_bit are used here
    SubGhzBlockGeneric generic;
    uint8_t pkt[MAZDA_V3_BYTES];
    bool valid;
};

static uint8_t mazda_v3_checksum(const uint8_t* pkt) {
    uint8_t c = 0;
    for(int i = 0; i < 4; i++) c ^= pkt[i]; // hop
    for(int i = 4; i < 7; i++) c ^= pkt[i]; // serial
    c ^= pkt[7]; // counter
    c ^= (pkt[8] >> 4) & 0xFu; // button nibble
    return c & 0xFu;
}

static void mazda_v3_extract_fields(SubGhzProtocolDecoderMazdaV3* instance) {
    const uint8_t* pkt = instance->pkt;
    uint32_t hop = ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16) | ((uint32_t)pkt[2] << 8) |
                   pkt[3];
    uint32_t serial = ((uint32_t)pkt[4] << 16) | ((uint32_t)pkt[5] << 8) | pkt[6];
    instance->generic.data = hop;
    instance->generic.serial = serial;
    instance->generic.cnt = pkt[7];
    instance->generic.btn = pkt[8] >> 4;
    uint8_t rx_ck = pkt[8] & 0xFu;
    instance->valid = (rx_ck == mazda_v3_checksum(pkt));
}

const SubGhzProtocolDecoder subghz_protocol_mazda_v3_decoder = {
    .alloc = subghz_protocol_decoder_mazda_v3_alloc,
    .free = subghz_protocol_decoder_mazda_v3_free,
    .reset = subghz_protocol_decoder_mazda_v3_reset,
    .feed = subghz_protocol_decoder_mazda_v3_feed,
    .get_hash_data = subghz_protocol_decoder_mazda_v3_get_hash_data,
    .serialize = subghz_protocol_decoder_mazda_v3_serialize,
    .deserialize = subghz_protocol_decoder_mazda_v3_deserialize,
    .get_string = subghz_protocol_decoder_mazda_v3_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_mazda_v3_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_mazda_v3 = {
    .name = SUBGHZ_PROTOCOL_MAZDA_V3_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_mazda_v3_decoder,
    .encoder = &subghz_protocol_mazda_v3_encoder,
};

void* subghz_protocol_decoder_mazda_v3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderMazdaV3* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_mazda_v3;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_MAZDA_V3_NAME;
    return instance;
}

void subghz_protocol_decoder_mazda_v3_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_mazda_v3_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaV3* instance = context;
    instance->decoder.parser_step = MazdaV3StepReset;
    instance->decoder.decode_count_bit = 0;
    memset(instance->pkt, 0, sizeof(instance->pkt));
    instance->valid = false;
}

static void mazda_v3_store_bit(SubGhzProtocolDecoderMazdaV3* instance, uint8_t bit) {
    uint16_t idx = instance->decoder.decode_count_bit;
    size_t byte = idx / 8u;
    uint8_t bitpos = 7u - (uint8_t)(idx % 8u);
    if(bit) {
        instance->pkt[byte] |= (uint8_t)(1u << bitpos);
    } else {
        instance->pkt[byte] &= (uint8_t)~(1u << bitpos);
    }
    instance->decoder.decode_count_bit++;
}

void subghz_protocol_decoder_mazda_v3_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaV3* instance = context;

    switch(instance->decoder.parser_step) {
    case MazdaV3StepReset:
        if(level && MAZDA_V3_IN_TOL(duration, MAZDA_V3_SYNC_HI)) {
            instance->decoder.parser_step = MazdaV3StepSyncLow;
        }
        break;

    case MazdaV3StepSyncLow:
        if(!level && MAZDA_V3_IN_TOL(duration, MAZDA_V3_SYNC_LO)) {
            instance->decoder.decode_count_bit = 0;
            memset(instance->pkt, 0, sizeof(instance->pkt));
            instance->decoder.parser_step = MazdaV3StepBitHigh;
        } else {
            instance->decoder.parser_step = MazdaV3StepReset;
        }
        break;

    case MazdaV3StepBitHigh:
        // the mark length is the same (~450us) for both bit values here; the
        // low (space) length right after it is what actually carries the bit
        if(level && MAZDA_V3_IN_TOL(duration, MAZDA_V3_HI)) {
            instance->decoder.parser_step = MazdaV3StepBitLow;
        } else {
            instance->decoder.parser_step = MazdaV3StepReset;
        }
        break;

    case MazdaV3StepBitLow: {
        if(level) {
            instance->decoder.parser_step = MazdaV3StepReset;
            break;
        }
        bool is_last_bit = (instance->decoder.decode_count_bit == MAZDA_V3_BITS - 1);
        uint8_t bit;
        bool ok;
        if(MAZDA_V3_IN_TOL(duration, MAZDA_V3_BIT1_LO)) {
            bit = 1;
            ok = true;
        } else if(
            MAZDA_V3_IN_TOL(duration, MAZDA_V3_BIT0_LO) ||
            (is_last_bit && duration >= MAZDA_V3_BIT0_LO * (100u - MAZDA_V3_TOL_PCT) / 100u)) {
            bit = 0;
            ok = true;
        } else {
            bit = 0;
            ok = false;
        }
        if(!ok) {
            instance->decoder.parser_step = MazdaV3StepReset;
            break;
        }

        mazda_v3_store_bit(instance, bit);

        if(instance->decoder.decode_count_bit == MAZDA_V3_BITS) {
            mazda_v3_extract_fields(instance);
            instance->decoder.parser_step = MazdaV3StepReset;
            if(instance->valid && instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
        } else {
            instance->decoder.parser_step = MazdaV3StepBitHigh;
        }
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_mazda_v3_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaV3* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < sizeof(instance->pkt); i++) hash = (hash * 33u) ^ instance->pkt[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_mazda_v3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaV3* instance = context;
    SubGhzProtocolStatus status =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(status != SubGhzProtocolStatusOk) return status;
    if(!flipper_format_write_hex(flipper_format, "Raw", instance->pkt, sizeof(instance->pkt)))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_mazda_v3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaV3* instance = context;
    flipper_format_rewind(flipper_format);
    FuriString* name = furi_string_alloc();
    bool match = flipper_format_read_string(flipper_format, "Protocol", name) &&
                 furi_string_equal_str(name, SUBGHZ_PROTOCOL_MAZDA_V3_NAME);
    furi_string_free(name);
    if(!match) return SubGhzProtocolStatusErrorParserOthers;
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_hex(flipper_format, "Raw", instance->pkt, sizeof(instance->pkt)))
        return SubGhzProtocolStatusErrorParserOthers;
    instance->decoder.decode_count_bit = (uint16_t)MAZDA_V3_BITS;
    mazda_v3_extract_fields(instance);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_mazda_v3_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaV3* instance = context;
    furi_string_cat_printf(
        output,
        "%s\r\n"
        "(EXPERIMENTAL / unverified)\r\n"
        "Hop:%08lX\r\n"
        "Serial:%06lX Cnt:%02lX\r\n"
        "Btn:%01X Checksum: %s\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.data,
        (unsigned long)instance->generic.serial,
        (unsigned long)instance->generic.cnt,
        instance->generic.btn,
        instance->valid ? "OK" : "FAIL");
}
