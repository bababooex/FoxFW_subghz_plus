#include "siemens_5wy72xx.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#include <furi.h>
#include <string.h>
#include <stdlib.h>

/*
 * Siemens 5WY72XX car remote (FCC ID M3N5WY72XX, the Siemens OEM remote of
 * DaimlerChrysler's SKREEK CS/RS platforms, 315 MHz), DECODE ONLY. Ported
 * from rtl_433's src/devices/siemens_5wy72xx.c (GPL-2.0), cross-checked
 * against the KAT project's port (github.com/KaraZajac/KAT, src/protocols/
 * siemens_5wy72xx.rs). No Flipper reference has this one; it is a
 * different remote from our siemens_vdo.c.
 *
 * Air format: Manchester (G.E. Thomas), Te=220us, reused via this
 * firmware's own lib/toolbox/manchester_decoder.c. Unlike most protocols in
 * this tree, there is no fixed-offset frame: a 0x6001 (16-bit) marker can
 * appear anywhere after an arbitrary lead-in, so this decoder keeps a
 * rolling 16-bit window over the decoded bitstream and only starts
 * collecting the 96-bit payload once that window matches.
 *
 * Frame (96 bits after the marker): a 32-bit ID (little-endian), an 8-bit
 * button-flag byte (0x01 Lock, 0x02 Unlock, 0x04 Trunk, 0x08 Panic, 0x10
 * Left Door, 0x20 Right Door - any combination), a 16-bit counter (big-
 * endian), 32 bits of encrypted data (shown in hex, not decrypted - see
 * microchip_hcs361.c for the same scope choice on a different chip), and
 * an 8-bit XOR checksum over the other 11 bytes. Also rejected: all-zero or
 * all-0xFF payloads (implausible/noise).
 *
 * No encoder, matching rtl_433 (receive only) - a captured signal can
 * still be replayed via Flipper's generic raw-replay.
 */

#define SIEMENS_TE_SHORT 220u
#define SIEMENS_TE_LONG 440u
#define SIEMENS_TE_DELTA 90u
#define SIEMENS_RESET_GAP 2000u
#define SIEMENS_SYNC_WORD 0x6001u
#define SIEMENS_PAYLOAD_BYTES 12u
#define SIEMENS_PAYLOAD_BITS (SIEMENS_PAYLOAD_BYTES * 8u)

#define SIEMENS_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < SIEMENS_TE_DELTA)

typedef enum {
    Siemens5wy72xxStepIdle = 0,
    Siemens5wy72xxStepData,
} Siemens5wy72xxDecoderStep;

struct SubGhzProtocolDecoderSiemens5wy72xx {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint16_t sync_window;
    bool have_sync;
    uint8_t buf[SIEMENS_PAYLOAD_BYTES];
    uint8_t bit_count;
    uint8_t encrypted[4]; /* the undecrypted 32-bit payload, for display only */
};

static const char* const siemens_5wy72xx_buttons[6] = {
    "Lock", "Unlock", "Trunk", "Panic", "Left Door", "Right Door"};

const SubGhzProtocolDecoder subghz_protocol_siemens_5wy72xx_decoder = {
    .alloc = subghz_protocol_decoder_siemens_5wy72xx_alloc,
    .free = subghz_protocol_decoder_siemens_5wy72xx_free,
    .reset = subghz_protocol_decoder_siemens_5wy72xx_reset,
    .feed = subghz_protocol_decoder_siemens_5wy72xx_feed,
    .get_hash_data = subghz_protocol_decoder_siemens_5wy72xx_get_hash_data,
    .serialize = subghz_protocol_decoder_siemens_5wy72xx_serialize,
    .deserialize = subghz_protocol_decoder_siemens_5wy72xx_deserialize,
    .get_string = subghz_protocol_decoder_siemens_5wy72xx_get_string,
};

/* Decode-only, as rtl_433. */
const SubGhzProtocolEncoder subghz_protocol_siemens_5wy72xx_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_siemens_5wy72xx = {
    .name = SUBGHZ_PROTOCOL_SIEMENS_5WY72XX_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_siemens_5wy72xx_decoder,
    .encoder = &subghz_protocol_siemens_5wy72xx_encoder,
};

static bool siemens_5wy72xx_fields(SubGhzProtocolDecoderSiemens5wy72xx* instance) {
    const uint8_t* b = instance->buf;
    uint32_t sum = 0;
    uint8_t x = 0;
    for(size_t i = 0; i < SIEMENS_PAYLOAD_BYTES; i++) {
        sum += b[i];
        x ^= b[i];
    }
    if(sum == 0 || sum == 0xFFu * SIEMENS_PAYLOAD_BYTES) return false; /* implausible */
    if(x != 0) return false; /* XOR check */

    instance->generic.serial = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
                                ((uint32_t)b[3] << 24);
    instance->generic.btn = b[4];
    instance->generic.cnt = ((uint32_t)b[5] << 8) | b[6];
    memcpy(instance->encrypted, &b[7], 4);

    uint64_t data = 0;
    for(size_t i = 0; i < 8; i++) data = (data << 8) | b[i];
    instance->generic.data = data;
    instance->generic.data_count_bit = 64; /* bytes 8..11 (4 encrypted + checksum) are
                                             * not persisted - see get_string */
    return true;
}

void* subghz_protocol_decoder_siemens_5wy72xx_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderSiemens5wy72xx* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_siemens_5wy72xx;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_SIEMENS_5WY72XX_NAME;
    return instance;
}

void subghz_protocol_decoder_siemens_5wy72xx_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_siemens_5wy72xx_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSiemens5wy72xx* instance = context;
    instance->decoder.parser_step = Siemens5wy72xxStepIdle;
    instance->sync_window = 0;
    instance->have_sync = false;
    instance->bit_count = 0;
}

void subghz_protocol_decoder_siemens_5wy72xx_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderSiemens5wy72xx* instance = context;

    if(duration > SIEMENS_RESET_GAP) {
        instance->decoder.parser_step = Siemens5wy72xxStepIdle;
        return;
    }

    ManchesterEvent event;
    if(SIEMENS_IN_TOL(duration, SIEMENS_TE_SHORT)) {
        event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    } else if(SIEMENS_IN_TOL(duration, SIEMENS_TE_LONG)) {
        event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    } else {
        instance->decoder.parser_step = Siemens5wy72xxStepIdle;
        return;
    }

    if(instance->decoder.parser_step == Siemens5wy72xxStepIdle) {
        instance->manchester_state = ManchesterStateMid1;
        manchester_advance(
            instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
        instance->sync_window = 0;
        instance->have_sync = false;
        instance->bit_count = 0;
        instance->decoder.parser_step = Siemens5wy72xxStepData;
    }

    bool bit;
    if(!manchester_advance(
           instance->manchester_state, event, &instance->manchester_state, &bit)) {
        return;
    }

    if(!instance->have_sync) {
        instance->sync_window = (uint16_t)((instance->sync_window << 1) | (bit ? 1u : 0u));
        if(instance->sync_window == SIEMENS_SYNC_WORD) {
            instance->have_sync = true;
            instance->bit_count = 0;
            memset(instance->buf, 0, sizeof(instance->buf));
        }
        return;
    }

    if(bit) instance->buf[instance->bit_count / 8] |= (uint8_t)(0x80 >> (instance->bit_count % 8));
    instance->bit_count++;

    if(instance->bit_count == SIEMENS_PAYLOAD_BITS) {
        bool valid = siemens_5wy72xx_fields(instance);
        instance->decoder.parser_step = Siemens5wy72xxStepIdle;
        if(valid && instance->base.callback)
            instance->base.callback(&instance->base, instance->base.context);
    }
}

uint8_t subghz_protocol_decoder_siemens_5wy72xx_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSiemens5wy72xx* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < sizeof(instance->buf); i++) hash = (hash * 33u) ^ instance->buf[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_siemens_5wy72xx_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderSiemens5wy72xx* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_siemens_5wy72xx_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderSiemens5wy72xx* instance = context;
    /* The trailing 4 encrypted bytes + checksum aren't persisted, so a
     * loaded key can't be re-checked here - see siemens_5wy72xx_fields. */
    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
    if(status != SubGhzProtocolStatusOk) return status;
    uint64_t data = instance->generic.data;
    uint8_t b[8];
    for(int i = 0; i < 8; i++) b[i] = (uint8_t)(data >> (8 * (7 - i)));
    instance->generic.serial =
        (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    instance->generic.btn = b[4];
    instance->generic.cnt = ((uint32_t)b[5] << 8) | b[6];
    memset(instance->encrypted, 0, sizeof(instance->encrypted));
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_siemens_5wy72xx_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderSiemens5wy72xx* instance = context;
    FuriString* btn_str = furi_string_alloc();
    uint8_t btn = instance->generic.btn;
    furi_string_cat_str(btn_str, "[");
    bool first = true;
    for(int i = 0; i < 6; i++) {
        if(btn & (1u << i)) {
            if(!first) furi_string_cat_str(btn_str, "; ");
            furi_string_cat_str(btn_str, siemens_5wy72xx_buttons[i]);
            first = false;
        }
    }
    furi_string_cat_str(btn_str, "]");
    furi_string_cat_printf(
        output,
        "%s 96bit\r\n"
        "Id:%08lX Btn:%02X %s\r\n"
        "Cnt:%04lX\r\n"
        "Enc:%02X%02X%02X%02X (not decrypted)\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        furi_string_get_cstr(btn_str),
        (unsigned long)instance->generic.cnt,
        instance->encrypted[0],
        instance->encrypted[1],
        instance->encrypted[2],
        instance->encrypted[3]);
    furi_string_free(btn_str);
}
