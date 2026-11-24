#include "cardin_s466.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"

#include <furi.h>
#include <stdlib.h>

/*
 * Cardin S466-TX2 garage-door remote - 24-bit fixed code (9 tri-state DIP
 * switches + a button), DECODE ONLY. Ported from rtl_433's src/devices/
 * cardin.c (r_device "Cardin S466-TX2", decoder 17, enabled by default
 * there), cross-checked against the KAT project's port (github.com/
 * KaraZajac/KAT, src/protocols/cardin_s466.rs). No Flipper firmware has
 * this one - RogueMaster's "Cardin S508" (our cardin_s508.c) is a
 * different, 868 MHz rolling-code remote from the same maker.
 *
 * The real remote transmits at 27.195 MHz; this decoder, like every
 * rtl_433 port, just runs on whatever frequency the radio is tuned to.
 *
 * Air format (AM/OOK): a sync pulse of >=6150us (the long "leading-gap"
 * PWM scheme's row marker), then 24 bits MSB-first, each bit encoded purely
 * by its OWN high (mark) pulse width - the low (space) that follows carries
 * no information in this scheme and is not checked: short (<1065us) = bit
 * 1, long (1065..3775us) = bit 0, anything >=3775us is read as a fresh sync
 * (restarting the frame) rather than a bit. No checksum: a button nibble of
 * 0x3/0x6/0x9/0xC and every DIP switch pair being a valid tri-state
 * combination (not the unused 4th combination) are the only checks.
 *
 * No encoder, matching rtl_433 (receive only) - a captured signal can still
 * be replayed via Flipper's generic raw-replay.
 */

#define CARDIN_S466_BITS 24u
#define CARDIN_S466_SYNC_MIN 3775u /* the reference's own short/long/sync split, not a window */
#define CARDIN_S466_SHORT_MAX 1065u

typedef enum {
    CardinS466StepIdle = 0,
    CardinS466StepData,
} CardinS466DecoderStep;

struct SubGhzProtocolDecoderCardinS466 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
};

const SubGhzProtocolDecoder subghz_protocol_cardin_s466_decoder = {
    .alloc = subghz_protocol_decoder_cardin_s466_alloc,
    .free = subghz_protocol_decoder_cardin_s466_free,
    .reset = subghz_protocol_decoder_cardin_s466_reset,
    .feed = subghz_protocol_decoder_cardin_s466_feed,
    .get_hash_data = subghz_protocol_decoder_cardin_s466_get_hash_data,
    .serialize = subghz_protocol_decoder_cardin_s466_serialize,
    .deserialize = subghz_protocol_decoder_cardin_s466_deserialize,
    .get_string = subghz_protocol_decoder_cardin_s466_get_string,
};

/* Decode-only, as rtl_433 - a captured signal is still replayable via raw. */
const SubGhzProtocolEncoder subghz_protocol_cardin_s466_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_cardin_s466 = {
    .name = SUBGHZ_PROTOCOL_CARDIN_S466_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_cardin_s466_decoder,
    .encoder = &subghz_protocol_cardin_s466_encoder,
};

/* Every DIP switch's (first, second) bit pair; DIP 6 (k=5) lives in byte 2,
 * the rest in bytes 0/1 at the same bit position. (false, true) is the
 * unused 4th tri-state combination and fails the gate. */
static bool cardin_s466_dip_ok(uint8_t b0, uint8_t b1, uint8_t b2) {
    static const uint8_t masks[9] = {8, 16, 32, 64, 128, 0, 1, 2, 4};
    for(int k = 0; k < 9; k++) {
        bool first, second;
        if(k == 5) {
            first = (b2 & 128) != 0;
            second = (b2 & 64) != 0;
        } else {
            first = (b0 & masks[k]) != 0;
            second = (b1 & masks[k]) != 0;
        }
        if(!first && second) return false;
    }
    return true;
}

static bool cardin_s466_fields(SubGhzBlockGeneric* generic, uint64_t data) {
    data &= 0xFFFFFFu;
    uint8_t b0 = (uint8_t)(data >> 16);
    uint8_t b1 = (uint8_t)(data >> 8);
    uint8_t b2 = (uint8_t)data;
    uint8_t btn_code = b2 & 0x3Fu;
    if(btn_code != 0x03 && btn_code != 0x06 && btn_code != 0x09 && btn_code != 0x0C)
        return false;
    if(!cardin_s466_dip_ok(b0, b1, b2)) return false;
    generic->data = data;
    generic->data_count_bit = CARDIN_S466_BITS;
    generic->serial = ((uint32_t)b0 << 10) | ((uint32_t)b1 << 2) | ((uint32_t)b2 >> 6);
    generic->btn = (uint8_t)((b2 & 0x0Fu) / 3u - 1u);
    return true;
}

void* subghz_protocol_decoder_cardin_s466_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderCardinS466* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_cardin_s466;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_CARDIN_S466_NAME;
    return instance;
}

void subghz_protocol_decoder_cardin_s466_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_cardin_s466_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderCardinS466* instance = context;
    instance->decoder.parser_step = CardinS466StepIdle;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

void subghz_protocol_decoder_cardin_s466_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderCardinS466* instance = context;

    /* The low (space) between pulses carries no information in this scheme
     * (see the file header) - only mark widths are classified. */
    if(!level) return;

    if(duration >= CARDIN_S466_SYNC_MIN) {
        /* A fresh sync - (re)start a frame. Also fires correctly on the
         * repeat's own leading sync, requiring one before every frame
         * rather than accumulating stray pulses without it. */
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->decoder.parser_step = CardinS466StepData;
        return;
    }

    if(instance->decoder.parser_step != CardinS466StepData) return;

    subghz_protocol_blocks_add_bit(&instance->decoder, duration < CARDIN_S466_SHORT_MAX ? 1 : 0);

    if(instance->decoder.decode_count_bit == CARDIN_S466_BITS) {
        bool valid = cardin_s466_fields(&instance->generic, instance->decoder.decode_data);
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        /* Require a fresh sync before the next frame - matches the real
         * remote, which sends one ahead of every repeat. */
        instance->decoder.parser_step = CardinS466StepIdle;
        if(valid && instance->base.callback)
            instance->base.callback(&instance->base, instance->base.context);
    }
}

uint8_t subghz_protocol_decoder_cardin_s466_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderCardinS466* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_cardin_s466_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderCardinS466* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_cardin_s466_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderCardinS466* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, CARDIN_S466_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    if(!cardin_s466_fields(&instance->generic, instance->generic.data))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_cardin_s466_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderCardinS466* instance = context;
    static const char* const rbutton[4] = {"11R", "10R", "01R", "00L?"};
    uint8_t btn = instance->generic.btn;
    furi_string_cat_printf(
        output,
        "%s 24bit\r\n"
        "Key:%06llX\r\n"
        "Sn:%05lX Btn:%s\r\n"
        "9 tri-state DIP switches, fixed code\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->generic.serial,
        rbutton[btn & 3u]);
}
