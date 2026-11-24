#include "isuzu_static.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#include <furi.h>
#include <stdlib.h>

/*
 * Isuzu Static: the remote of an Isuzu FTS truck, decode + a simple
 * replay encoder. KAT's own decoder (github.com/KaraZajac/KAT, src/
 * protocols/isuzu_static.rs), worked out from two real recordings (two
 * presses each, lock and unlock); no other reference reads this signal
 * and no FCC filing has been matched to it.
 *
 * Air format: Manchester (carrier then pause is a 1), Te=523.5us. A press
 * sends, back to back with NO usable gap between copies (the period is
 * ~70.2ms, the frame itself ~69.1ms - under 1.2ms of actual silence,
 * smaller than some in-frame half-cells), a 64-bit frame followed by two
 * 1 bits, over and over. Because there is no reliable frame boundary to
 * reset on, this decoder keeps a continuously-sliding 66-bit window
 * (updated one Manchester bit at a time, never reset mid-press) and
 * reports whenever the window's own trailing 2 bits are "11" - which a
 * random 64-bit payload would only coincidentally satisfy on its own
 * (checked against the fixed byte values below too), while a genuine
 * frame boundary satisfies it by construction every cycle. A short
 * cooldown after each report keeps one accepted frame from being reported
 * again a few shifts later by the same still-sliding window.
 *
 * Frame (64 bits / 8 bytes): bytes 0-3 fixed (01 10 01 83 - the same in
 * both recordings, so either the fob's ID or a protocol constant - one
 * remote can't tell), byte 4's high nibble the button (not independently
 * confirmed beyond "two recordings, two different values"), its low
 * nibble changes with no pattern across just four presses, bytes 5-7
 * fixed (0E 81 FF). No checksum; the code never changes, so transmit is
 * just a clean replay of the captured frame.
 */

#define ISUZU_STATIC_TE 523u
#define ISUZU_STATIC_TOL 120u
#define ISUZU_STATIC_RESET_GAP 3000u
#define ISUZU_STATIC_REPORT_COOLDOWN 50u
#define ISUZU_STATIC_BITS 64u
#define ISUZU_STATIC_TX_COPIES 8u

#define ISUZU_STATIC_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < ISUZU_STATIC_TOL)
#define ISUZU_STATIC_FIXED_HEAD 0x01100183u
#define ISUZU_STATIC_FIXED_TAIL 0x0E81FFu

struct SubGhzProtocolDecoderIsuzuStatic {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint64_t lo64; /* the newest 64 bits of the sliding 66-bit window */
    uint8_t hi2; /* the 2 oldest bits of the sliding 66-bit window */
    uint16_t cooldown;
};

struct SubGhzProtocolEncoderIsuzuStatic {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    bool overflow;
};

const SubGhzProtocolDecoder subghz_protocol_isuzu_static_decoder = {
    .alloc = subghz_protocol_decoder_isuzu_static_alloc,
    .free = subghz_protocol_decoder_isuzu_static_free,
    .reset = subghz_protocol_decoder_isuzu_static_reset,
    .feed = subghz_protocol_decoder_isuzu_static_feed,
    .get_hash_data = subghz_protocol_decoder_isuzu_static_get_hash_data,
    .serialize = subghz_protocol_decoder_isuzu_static_serialize,
    .deserialize = subghz_protocol_decoder_isuzu_static_deserialize,
    .get_string = subghz_protocol_decoder_isuzu_static_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_isuzu_static_encoder = {
    .alloc = subghz_protocol_encoder_isuzu_static_alloc,
    .free = subghz_protocol_encoder_isuzu_static_free,
    .deserialize = subghz_protocol_encoder_isuzu_static_deserialize,
    .stop = subghz_protocol_encoder_isuzu_static_stop,
    .yield = subghz_protocol_encoder_isuzu_static_yield,
};

const SubGhzProtocol subghz_protocol_isuzu_static = {
    .name = SUBGHZ_PROTOCOL_ISUZU_STATIC_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_isuzu_static_decoder,
    .encoder = &subghz_protocol_isuzu_static_encoder,
};

static bool isuzu_static_valid(uint64_t data64) {
    if((data64 >> 32) != ISUZU_STATIC_FIXED_HEAD) return false;
    if((data64 & 0xFFFFFFu) != ISUZU_STATIC_FIXED_TAIL) return false;
    return true;
}

static void isuzu_static_fields(SubGhzBlockGeneric* generic, uint64_t data64) {
    generic->data = data64;
    generic->data_count_bit = ISUZU_STATIC_BITS;
    generic->btn = (uint8_t)((data64 >> 28) & 0xFu);
}

void* subghz_protocol_decoder_isuzu_static_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderIsuzuStatic* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_isuzu_static;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_ISUZU_STATIC_NAME;
    return instance;
}

void subghz_protocol_decoder_isuzu_static_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_isuzu_static_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuStatic* instance = context;
    instance->lo64 = 0;
    instance->hi2 = 0;
    instance->cooldown = 0;
    instance->manchester_state = ManchesterStateMid1;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_isuzu_static_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuStatic* instance = context;

    if(duration > ISUZU_STATIC_RESET_GAP) {
        subghz_protocol_decoder_isuzu_static_reset(instance);
        return;
    }

    ManchesterEvent event;
    if(ISUZU_STATIC_IN_TOL(duration, ISUZU_STATIC_TE)) {
        event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    } else if(ISUZU_STATIC_IN_TOL(duration, 2u * ISUZU_STATIC_TE)) {
        event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    } else {
        subghz_protocol_decoder_isuzu_static_reset(instance);
        return;
    }

    bool bit;
    if(!manchester_advance(instance->manchester_state, event, &instance->manchester_state, &bit)) {
        return;
    }

    bool carry = (instance->lo64 >> 63) & 1u;
    instance->lo64 = (instance->lo64 << 1) | (bit ? 1u : 0u);
    instance->hi2 = (uint8_t)(((instance->hi2 << 1) | (carry ? 1u : 0u)) & 0x3u);

    if(instance->cooldown > 0) {
        instance->cooldown--;
        return;
    }

    if((instance->lo64 & 0x3u) == 0x3u) {
        uint64_t data64 = ((uint64_t)instance->hi2 << 62) | (instance->lo64 >> 2);
        if(isuzu_static_valid(data64)) {
            isuzu_static_fields(&instance->generic, data64);
            instance->cooldown = ISUZU_STATIC_REPORT_COOLDOWN;
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
    }
}

uint8_t subghz_protocol_decoder_isuzu_static_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuStatic* instance = context;
    return subghz_protocol_blocks_get_hash_data(&instance->decoder, 8);
}

SubGhzProtocolStatus subghz_protocol_decoder_isuzu_static_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuStatic* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_isuzu_static_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuStatic* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, ISUZU_STATIC_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    if(!isuzu_static_valid(instance->generic.data)) return SubGhzProtocolStatusErrorParserOthers;
    isuzu_static_fields(&instance->generic, instance->generic.data);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_isuzu_static_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuStatic* instance = context;
    furi_string_cat_printf(
        output,
        "%s 64bit\r\n"
        "Key:%016llX\r\n"
        "Btn:%01X\r\n"
        "Fixed code - no counter\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        instance->generic.btn);
}

/* --- Encoder: a clean replay, the code never changes --- */

#define ISUZU_STATIC_UPLOAD_CAPACITY ((ISUZU_STATIC_BITS + 2u) * 2u * ISUZU_STATIC_TX_COPIES + 4u)

static void
    isuzu_static_emit(SubGhzProtocolEncoderIsuzuStatic* instance, bool level, uint32_t duration) {
    size_t size = instance->encoder.size_upload;
    LevelDuration* upload = instance->encoder.upload;
    if(size && level_duration_get_level(upload[size - 1]) == level) {
        upload[size - 1] =
            level_duration_make(level, duration + level_duration_get_duration(upload[size - 1]));
    } else if(size < ISUZU_STATIC_UPLOAD_CAPACITY) {
        upload[size] = level_duration_make(level, duration);
        instance->encoder.size_upload++;
    } else {
        instance->overflow = true;
    }
}

/* One Manchester bit: carrier then pause for a 1, pause then carrier for a 0. */
static void isuzu_static_emit_bit(SubGhzProtocolEncoderIsuzuStatic* instance, bool one) {
    if(one) {
        isuzu_static_emit(instance, true, ISUZU_STATIC_TE);
        isuzu_static_emit(instance, false, ISUZU_STATIC_TE);
    } else {
        isuzu_static_emit(instance, false, ISUZU_STATIC_TE);
        isuzu_static_emit(instance, true, ISUZU_STATIC_TE);
    }
}

static void isuzu_static_transmit(SubGhzProtocolEncoderIsuzuStatic* instance, uint64_t data64) {
    for(uint32_t copy = 0; copy < ISUZU_STATIC_TX_COPIES; copy++) {
        for(int i = 63; i >= 0; i--) isuzu_static_emit_bit(instance, (data64 >> i) & 1u);
        isuzu_static_emit_bit(instance, true);
        isuzu_static_emit_bit(instance, true);
    }
}

void* subghz_protocol_encoder_isuzu_static_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderIsuzuStatic* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_isuzu_static;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_ISUZU_STATIC_NAME;
    instance->encoder.upload = malloc(ISUZU_STATIC_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    return instance;
}

void subghz_protocol_encoder_isuzu_static_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderIsuzuStatic* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_isuzu_static_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderIsuzuStatic* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_isuzu_static_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderIsuzuStatic* instance = context;
    if(!instance->encoder.is_running || !instance->encoder.repeat) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }
    LevelDuration value = instance->encoder.upload[instance->encoder.front++];
    if(instance->encoder.front == instance->encoder.size_upload) {
        instance->encoder.front = 0;
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
    }
    return value;
}

SubGhzProtocolStatus subghz_protocol_encoder_isuzu_static_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderIsuzuStatic* instance = context;
    subghz_protocol_encoder_isuzu_static_stop(instance);

    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, ISUZU_STATIC_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    if(!isuzu_static_valid(instance->generic.data)) return SubGhzProtocolStatusErrorParserOthers;

    instance->encoder.size_upload = 0;
    instance->overflow = false;
    isuzu_static_transmit(instance, instance->generic.data);
    if(instance->overflow) return SubGhzProtocolStatusErrorEncoderGetUpload;

    uint32_t repeat = 1;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Repeat", &repeat, 1);
    instance->encoder.repeat = repeat;

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
