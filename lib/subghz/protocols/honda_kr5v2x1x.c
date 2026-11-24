#include "honda_kr5v2x1x.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#include <furi.h>
#include <string.h>
#include <stdlib.h>

/*
 * Honda Keyfob KR5V2X/KR5V1X, DECODE ONLY. Ported from rtl_433's
 * src/devices/continental_car_remote.c (honda_keyfob_decode, GPL-2.0;
 * r_device "Honda Keyfob KR5V2X/1X"), cross-checked against the KAT
 * project's port (github.com/KaraZajac/KAT, src/protocols/
 * honda_kr5v2x1x.rs). rtl_433 added this as an ALTERNATE decoding of the
 * same physical remotes as our continental_rke.c - the two disagree on
 * bit layout and checksum, so both are kept.
 *
 * NO REAL CAPTURE VALIDATES THIS ANYWHERE - not in rtl_433's own test
 * suite, not in KAT's recording library. Both projects' tests build frames
 * from the reference C's own logic (self-consistency only). Treat a
 * "valid" decode here with real caution until confirmed against an actual
 * capture.
 *
 * Air format: FSK Manchester (zero-bit variant), Te=60us - fast for this
 * firmware's usual GPIO-edge capture; feasibility on real Flipper hardware
 * is unconfirmed.
 *
 * Frame: a fixed 24-bit manufacturer code (0xEC0F62) found anywhere in the
 * message (not at a fixed offset - same rolling-window search as
 * siemens_5wy72xx.c's sync word, just 24 bits here), then 120 more bits
 * starting from that code's OWN last byte (0x62 becomes frame byte 0):
 * byte 1 a packet-copy index (not shown), bytes 2..5 a 32-bit device ID,
 * byte 6 the button/event code, bytes 7..9 a 24-bit counter, bytes 10..13
 * a 32-bit rolling code (cipher unknown), byte 14 a CRC-8 (OpenSafety
 * polynomial 0x2F, init 0) over bytes 0..13.
 */

#define HONDA_KR5_TE 60u
#define HONDA_KR5_TOL 30u
#define HONDA_KR5_CODE 0xEC0F62u /* 24 bits */
#define HONDA_KR5_CODE_BITS 24u
#define HONDA_KR5_FRAME_BYTES 15u /* 120 bits */
#define HONDA_KR5_FRAME_BITS (HONDA_KR5_FRAME_BYTES * 8u)
#define HONDA_KR5_RESET_GAP 1500u

#define HONDA_KR5_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < HONDA_KR5_TOL)

typedef enum {
    HondaKr5StepSearching = 0,
    HondaKr5StepCollecting,
} HondaKr5DecoderStep;

struct SubGhzProtocolDecoderHondaKr5v2x1x {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint32_t window; /* rolling last-24-bits window, for the code match */
    uint8_t frame[HONDA_KR5_FRAME_BYTES];
    uint8_t count;
};

static const char* honda_kr5_event_name(uint8_t code) {
    switch(code) {
    case 0x21:
        return "Lock";
    case 0x22:
        return "Unlock";
    case 0x24:
        return "Trunk";
    case 0x27:
        return "Emergency";
    case 0x2D:
        return "RemoteStart";
    default:
        return "?";
    }
}

/* Standard MSB-first, non-reflected CRC-8 (OpenSafety polynomial 0x2F). */
static uint8_t honda_kr5_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x2F) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

const SubGhzProtocolDecoder subghz_protocol_honda_kr5v2x1x_decoder = {
    .alloc = subghz_protocol_decoder_honda_kr5v2x1x_alloc,
    .free = subghz_protocol_decoder_honda_kr5v2x1x_free,
    .reset = subghz_protocol_decoder_honda_kr5v2x1x_reset,
    .feed = subghz_protocol_decoder_honda_kr5v2x1x_feed,
    .get_hash_data = subghz_protocol_decoder_honda_kr5v2x1x_get_hash_data,
    .serialize = subghz_protocol_decoder_honda_kr5v2x1x_serialize,
    .deserialize = subghz_protocol_decoder_honda_kr5v2x1x_deserialize,
    .get_string = subghz_protocol_decoder_honda_kr5v2x1x_get_string,
};

/* Decode-only, as rtl_433. */
const SubGhzProtocolEncoder subghz_protocol_honda_kr5v2x1x_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_honda_kr5v2x1x = {
    .name = SUBGHZ_PROTOCOL_HONDA_KR5V2X1X_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_honda_kr5v2x1x_decoder,
    .encoder = &subghz_protocol_honda_kr5v2x1x_encoder,
};

static bool honda_kr5_fields(SubGhzBlockGeneric* generic, const uint8_t* f) {
    if(honda_kr5_crc8(f, 14) != f[14]) return false;
    generic->serial = ((uint32_t)f[2] << 24) | ((uint32_t)f[3] << 16) | ((uint32_t)f[4] << 8) | f[5];
    generic->btn = f[6];
    generic->cnt = ((uint32_t)f[7] << 16) | ((uint32_t)f[8] << 8) | f[9];
    uint64_t rolling = 0;
    for(int i = 10; i < 14; i++) rolling = (rolling << 8) | f[i];
    generic->data = rolling;
    generic->data_count_bit = 32;
    return true;
}

void* subghz_protocol_decoder_honda_kr5v2x1x_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHondaKr5v2x1x* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_honda_kr5v2x1x;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_HONDA_KR5V2X1X_NAME;
    return instance;
}

void subghz_protocol_decoder_honda_kr5v2x1x_free(void* context) {
    furi_check(context);
    free(context);
}

static void honda_kr5_reset_search(SubGhzProtocolDecoderHondaKr5v2x1x* instance) {
    instance->decoder.parser_step = HondaKr5StepSearching;
    instance->window = 0;
    instance->manchester_state = ManchesterStateMid1;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_honda_kr5v2x1x_reset(void* context) {
    furi_check(context);
    honda_kr5_reset_search(context);
}

void subghz_protocol_decoder_honda_kr5v2x1x_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderHondaKr5v2x1x* instance = context;

    if(duration > HONDA_KR5_RESET_GAP) {
        honda_kr5_reset_search(instance);
        return;
    }

    ManchesterEvent event;
    if(HONDA_KR5_IN_TOL(duration, HONDA_KR5_TE)) {
        event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    } else if(HONDA_KR5_IN_TOL(duration, 2u * HONDA_KR5_TE)) {
        event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    } else {
        honda_kr5_reset_search(instance);
        return;
    }

    bool bit;
    if(!manchester_advance(
           instance->manchester_state, event, &instance->manchester_state, &bit)) {
        return;
    }

    if(instance->decoder.parser_step == HondaKr5StepCollecting) {
        if(bit) instance->frame[instance->count / 8] |= (uint8_t)(0x80 >> (instance->count % 8));
        instance->count++;
        if(instance->count == HONDA_KR5_FRAME_BITS) {
            bool valid = honda_kr5_fields(&instance->generic, instance->frame);
            honda_kr5_reset_search(instance);
            if(valid && instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        return;
    }

    uint32_t candidate = ((instance->window << 1) | (bit ? 1u : 0u)) & 0xFFFFFFu;
    if(candidate == HONDA_KR5_CODE) {
        instance->decoder.parser_step = HondaKr5StepCollecting;
        memset(instance->frame, 0, sizeof(instance->frame));
        instance->frame[0] = (uint8_t)(HONDA_KR5_CODE & 0xFFu); /* 0x62, the code's last byte */
        instance->count = 8;
        return;
    }
    instance->window = candidate;
}

uint8_t subghz_protocol_decoder_honda_kr5v2x1x_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHondaKr5v2x1x* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < sizeof(instance->frame); i++) hash = (hash * 33u) ^ instance->frame[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_kr5v2x1x_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderHondaKr5v2x1x* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_kr5v2x1x_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderHondaKr5v2x1x* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 32);
}

void subghz_protocol_decoder_honda_kr5v2x1x_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderHondaKr5v2x1x* instance = context;
    furi_string_cat_printf(
        output,
        "%s 120bit (UNVALIDATED - no real\r\n"
        "capture exists anywhere)\r\n"
        "Id:%08lX Btn:%02X %s Cnt:%06lX\r\n"
        "Rolling:%08lX cipher unknown\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        honda_kr5_event_name(instance->generic.btn),
        (unsigned long)instance->generic.cnt,
        (unsigned long)instance->generic.data);
}
