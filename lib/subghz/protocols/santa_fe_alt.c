#include "santa_fe_alt.h"

#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <stdlib.h>
#include <string.h>

/*
 * SantaFe Alt - EXPERIMENTAL, UNVALIDATED DECODER.
 *
 * Ported on request from the untested scaffolding in auto_rke_protocols.c
 * into the normal decoder shape, decode-only. No cited source, no real
 * capture behind the numbers below - treat every one as a placeholder
 * until confirmed against an actual capture. Distinct from the already-
 * registered "SantaFe 13-16" protocol (santa_fe.c).
 *
 * Frame is 10 bytes (80 bits) - too wide for SubGhzBlockDecoder's 64-bit
 * decode_data, so bits are packed directly into a local byte array, same
 * approach as mazda_v3.c.
 */

#define SFE_ALT_BITS     80u
#define SFE_ALT_BYTES    10u
#define SFE_ALT_SYNC_HI  375u
#define SFE_ALT_SYNC_LO  12000u
#define SFE_ALT_HI       375u /* constant mark length for both bit values */
#define SFE_ALT_BIT1_LO  125u
#define SFE_ALT_BIT0_LO  375u
#define SFE_ALT_TOL_PCT  25u

typedef enum {
    SfeAltStepReset = 0,
    SfeAltStepSyncLow,
    SfeAltStepBitHigh,
    SfeAltStepBitLow,
} SfeAltDecoderStep;

struct SubGhzProtocolDecoderSantaFeAlt {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder; // only parser_step / decode_count_bit are used here
    SubGhzBlockGeneric generic;
    uint8_t pkt[SFE_ALT_BYTES];
    bool valid;
};

static uint8_t sfe_alt_crc8(const uint8_t* data, uint32_t len) {
    uint8_t crc = 0xFFu;
    for(uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++) {
            if(crc & 0x80u)
                crc = (uint8_t)((crc << 1) ^ 0x31u);
            else
                crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void sfe_alt_extract_fields(SubGhzProtocolDecoderSantaFeAlt* instance) {
    const uint8_t* pkt = instance->pkt;
    uint32_t rolling = ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16) |
                        ((uint32_t)pkt[2] << 8) | pkt[3];
    uint32_t serial = ((uint32_t)pkt[4] << 16) | ((uint32_t)pkt[5] << 8) | pkt[6];
    instance->generic.data = rolling;
    instance->generic.serial = serial;
    instance->generic.cnt = pkt[7];
    instance->generic.btn = pkt[8];
    uint8_t rx_crc = pkt[9];
    instance->valid = (rx_crc == sfe_alt_crc8(pkt, 9));
}

const SubGhzProtocolDecoder subghz_protocol_santa_fe_alt_decoder = {
    .alloc = subghz_protocol_decoder_santa_fe_alt_alloc,
    .free = subghz_protocol_decoder_santa_fe_alt_free,
    .reset = subghz_protocol_decoder_santa_fe_alt_reset,
    .feed = subghz_protocol_decoder_santa_fe_alt_feed,
    .get_hash_data = subghz_protocol_decoder_santa_fe_alt_get_hash_data,
    .serialize = subghz_protocol_decoder_santa_fe_alt_serialize,
    .deserialize = subghz_protocol_decoder_santa_fe_alt_deserialize,
    .get_string = subghz_protocol_decoder_santa_fe_alt_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_santa_fe_alt_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_santa_fe_alt = {
    .name = SUBGHZ_PROTOCOL_SANTA_FE_ALT_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_santa_fe_alt_decoder,
    .encoder = &subghz_protocol_santa_fe_alt_encoder,
};

void* subghz_protocol_decoder_santa_fe_alt_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderSantaFeAlt* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_santa_fe_alt;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_SANTA_FE_ALT_NAME;
    return instance;
}

void subghz_protocol_decoder_santa_fe_alt_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_santa_fe_alt_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSantaFeAlt* instance = context;
    instance->decoder.parser_step = SfeAltStepReset;
    instance->decoder.decode_count_bit = 0;
    memset(instance->pkt, 0, sizeof(instance->pkt));
    instance->valid = false;
}

static void sfe_alt_store_bit(SubGhzProtocolDecoderSantaFeAlt* instance, uint8_t bit) {
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

void subghz_protocol_decoder_santa_fe_alt_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderSantaFeAlt* instance = context;

#define SFE_ALT_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) <= ((ref) * SFE_ALT_TOL_PCT / 100u))

    switch(instance->decoder.parser_step) {
    case SfeAltStepReset:
        if(level && SFE_ALT_IN_TOL(duration, SFE_ALT_SYNC_HI)) {
            instance->decoder.parser_step = SfeAltStepSyncLow;
        }
        break;

    case SfeAltStepSyncLow:
        if(!level && SFE_ALT_IN_TOL(duration, SFE_ALT_SYNC_LO)) {
            instance->decoder.decode_count_bit = 0;
            memset(instance->pkt, 0, sizeof(instance->pkt));
            instance->decoder.parser_step = SfeAltStepBitHigh;
        } else {
            instance->decoder.parser_step = SfeAltStepReset;
        }
        break;

    case SfeAltStepBitHigh:
        if(level && SFE_ALT_IN_TOL(duration, SFE_ALT_HI)) {
            instance->decoder.parser_step = SfeAltStepBitLow;
        } else {
            instance->decoder.parser_step = SfeAltStepReset;
        }
        break;

    case SfeAltStepBitLow: {
        if(level) {
            instance->decoder.parser_step = SfeAltStepReset;
            break;
        }
        bool is_last_bit = (instance->decoder.decode_count_bit == SFE_ALT_BITS - 1);
        uint8_t bit;
        bool ok;
        if(SFE_ALT_IN_TOL(duration, SFE_ALT_BIT1_LO)) {
            bit = 1;
            ok = true;
        } else if(
            SFE_ALT_IN_TOL(duration, SFE_ALT_BIT0_LO) ||
            (is_last_bit && duration >= SFE_ALT_BIT0_LO * (100u - SFE_ALT_TOL_PCT) / 100u)) {
            bit = 0;
            ok = true;
        } else {
            bit = 0;
            ok = false;
        }
        if(!ok) {
            instance->decoder.parser_step = SfeAltStepReset;
            break;
        }

        sfe_alt_store_bit(instance, bit);

        if(instance->decoder.decode_count_bit == SFE_ALT_BITS) {
            sfe_alt_extract_fields(instance);
            instance->decoder.parser_step = SfeAltStepReset;
            if(instance->valid && instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
        } else {
            instance->decoder.parser_step = SfeAltStepBitHigh;
        }
        break;
    }
    }

#undef SFE_ALT_IN_TOL
}

uint8_t subghz_protocol_decoder_santa_fe_alt_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSantaFeAlt* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < sizeof(instance->pkt); i++) hash = (hash * 33u) ^ instance->pkt[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_santa_fe_alt_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderSantaFeAlt* instance = context;
    SubGhzProtocolStatus status =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(status != SubGhzProtocolStatusOk) return status;
    if(!flipper_format_write_hex(flipper_format, "Raw", instance->pkt, sizeof(instance->pkt)))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_protocol_decoder_santa_fe_alt_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderSantaFeAlt* instance = context;
    flipper_format_rewind(flipper_format);
    FuriString* name = furi_string_alloc();
    bool match = flipper_format_read_string(flipper_format, "Protocol", name) &&
                 furi_string_equal_str(name, SUBGHZ_PROTOCOL_SANTA_FE_ALT_NAME);
    furi_string_free(name);
    if(!match) return SubGhzProtocolStatusErrorParserOthers;
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_hex(flipper_format, "Raw", instance->pkt, sizeof(instance->pkt)))
        return SubGhzProtocolStatusErrorParserOthers;
    instance->decoder.decode_count_bit = (uint8_t)SFE_ALT_BITS;
    sfe_alt_extract_fields(instance);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_santa_fe_alt_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderSantaFeAlt* instance = context;
    furi_string_cat_printf(
        output,
        "%s\r\n"
        "(EXPERIMENTAL / unverified)\r\n"
        "Rolling:%08lX\r\n"
        "Serial:%06lX Cnt:%02lX\r\n"
        "Btn:%02X Checksum: %s\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.data,
        (unsigned long)instance->generic.serial,
        (unsigned long)instance->generic.cnt,
        instance->generic.btn,
        instance->valid ? "OK" : "FAIL");
}
