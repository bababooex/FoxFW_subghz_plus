#include "vag_v1.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include <lib/subghz/blocks/custom_btn_i.h>

#include <furi.h>
#include <stdlib.h>

/*
 * VW-1: the oldest Volkswagen Group rolling-code remote (VW/Audi/Seat/Skoda,
 * ~1998-2005). Real scheme, not a guess - this is the "VW-1" analyzed in
 * Garcia, Oswald, Kasper & Pavlides, "Lock It and Still Lose It" (USENIX
 * Security 2016, section 3.2): there is no secret key and no checksum, only
 * an obfuscation of the transponder UID and a 24-bit LFSR. A single capture
 * gives the UID and the register's current state; the next valid code is
 * the register advanced a fixed number of ticks.
 *
 * Ported from the KAT project's independent re-derivation of the same
 * scheme (github.com/KaraZajac/KAT, src/protocols/vw_1.rs), which the
 * project worked out from real recordings of one VW flip key (transmitter
 * labelled HLO 1J0 959 753 A, Golf Mk4 / Bora generation) and confirmed the
 * LFSR/obfuscation/button layout against ~628 captured presses. The air
 * timing, bit layout, polynomial and the fixed 23-tick-per-press step below
 * all come from that port; none of it is this file's own guesswork (unlike
 * the placeholder this file used to carry).
 *
 * This is NOT the same protocol as vag.c (the newer 80-bit AUT64/XTEA frame
 * at 434.4 MHz) or vag_polo.c.
 *
 * Frame (60 bits, LSB-first on the wire):
 *   bits 0..32  UID, obfuscated: each of the 4 bytes XORed with m (below)
 *   bits 32..56 the 24-bit LFSR state (as 3 raw bytes)
 *   bits 56..60 button nibble: 1 = Unlock, 2 = Lock
 * m = state_byte0 ^ state_byte1 ^ state_byte2 (the obfuscation mask - XOR it
 * into the first four bytes again to recover the UID).
 *
 * The encoder has not been tried against a real car (no reference capture
 * of a resynced remote to check against) - but the scheme itself is public,
 * cited research with real test vectors, not speculation.
 */

#define VAG_V1_UNIT        500u
#define VAG_V1_BITS        60u
#define VAG_V1_POLY        0x1000087u /* x^24 + x^7 + x^2 + x + 1 */
#define VAG_V1_STEP_TICKS  23u
#define VAG_V1_BTN_UNLOCK  1u
#define VAG_V1_BTN_LOCK    2u
#define VAG_V1_COPIES      3u
#define VAG_V1_COPY_GAP    10000u
#define VAG_V1_FINAL_GAP   100000u
#define VAG_V1_GAP_MIN     2500u
#define VAG_V1_UPLOAD_CAPACITY (VAG_V1_COPIES * (4u + 2u * VAG_V1_BITS) + 8u)

static inline bool vag_v1_is_short(uint32_t d) {
    return d >= 250u && d <= 750u;
}
static inline bool vag_v1_is_long(uint32_t d) {
    return d >= 751u && d <= 1250u;
}
static inline bool vag_v1_is_start(uint32_t d) {
    return d >= 1251u && d <= 2200u;
}

typedef enum {
    VagV1StepIdle = 0,
    VagV1StepStartLow,
    VagV1StepBitHigh,
    VagV1StepBitLowShort, /* carrier before this pause was short -> bit will be 0 */
    VagV1StepBitLowLong,  /* carrier before this pause was long -> bit will be 1 */
    VagV1StepStop,
    VagV1StepGap,
} VagV1DecoderStep;

struct SubGhzProtocolDecoderVagV1 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint64_t frame;
    uint8_t count;
};

struct SubGhzProtocolEncoderVagV1 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    bool overflow;
};

/* One LFSR clock: shift left, and on carry out of bit 24 XOR the feedback taps. */
static uint32_t vag_v1_tick(uint32_t state) {
    state <<= 1;
    if(state & 0x1000000u) state ^= VAG_V1_POLY;
    return state & 0xFFFFFFu;
}

/* The register advanced one press (23 ticks). */
static uint32_t vag_v1_step(uint32_t state, uint32_t presses) {
    for(uint32_t p = 0; p < presses; p++) {
        for(uint32_t i = 0; i < VAG_V1_STEP_TICKS; i++) state = vag_v1_tick(state);
    }
    return state;
}

static inline uint8_t vag_v1_frame_byte(uint64_t frame, uint32_t k) {
    return (uint8_t)((frame >> (8u * k)) & 0xFFu);
}

/* Parses UID/state/button out of a 60-bit frame. Returns false (gate fails)
 * on a zero state or an unknown button - matches the real remote's frames,
 * which never have either. */
static bool vag_v1_parse(uint64_t frame, uint32_t* uid, uint32_t* state, uint8_t* button) {
    uint8_t s0 = vag_v1_frame_byte(frame, 4);
    uint8_t s1 = vag_v1_frame_byte(frame, 5);
    uint8_t s2 = vag_v1_frame_byte(frame, 6);
    uint32_t st = (uint32_t)s0 | ((uint32_t)s1 << 8) | ((uint32_t)s2 << 16);
    uint8_t btn = (uint8_t)((frame >> 56) & 0xFu);
    if(st == 0 || (btn != VAG_V1_BTN_UNLOCK && btn != VAG_V1_BTN_LOCK)) return false;
    uint8_t m = s0 ^ s1 ^ s2;
    *uid = ((uint32_t)(vag_v1_frame_byte(frame, 0) ^ m) << 24) |
           ((uint32_t)(vag_v1_frame_byte(frame, 1) ^ m) << 16) |
           ((uint32_t)(vag_v1_frame_byte(frame, 2) ^ m) << 8) |
           (uint32_t)(vag_v1_frame_byte(frame, 3) ^ m);
    *state = st;
    *button = btn;
    return true;
}

/* The inverse of vag_v1_parse: rebuilds the 60 frame bits from the fields. */
static uint64_t vag_v1_build(uint32_t uid, uint32_t state, uint8_t button) {
    uint8_t s0 = (uint8_t)state;
    uint8_t s1 = (uint8_t)(state >> 8);
    uint8_t s2 = (uint8_t)(state >> 16);
    uint8_t m = s0 ^ s1 ^ s2;
    uint8_t by[7] = {
        (uint8_t)((uid >> 24) ^ m),
        (uint8_t)((uid >> 16) ^ m),
        (uint8_t)((uid >> 8) ^ m),
        (uint8_t)(uid ^ m),
        s0,
        s1,
        s2,
    };
    uint64_t frame = 0;
    for(size_t k = 0; k < 7; k++) frame |= (uint64_t)by[k] << (8u * k);
    frame |= (uint64_t)(button & 0xFu) << 56;
    return frame;
}

const SubGhzProtocolDecoder subghz_protocol_vag_v1_decoder = {
    .alloc = subghz_protocol_decoder_vag_v1_alloc,
    .free = subghz_protocol_decoder_vag_v1_free,
    .reset = subghz_protocol_decoder_vag_v1_reset,
    .feed = subghz_protocol_decoder_vag_v1_feed,
    .get_hash_data = subghz_protocol_decoder_vag_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_vag_v1_serialize,
    .deserialize = subghz_protocol_decoder_vag_v1_deserialize,
    .get_string = subghz_protocol_decoder_vag_v1_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_vag_v1_encoder = {
    .alloc = subghz_protocol_encoder_vag_v1_alloc,
    .free = subghz_protocol_encoder_vag_v1_free,
    .deserialize = subghz_protocol_encoder_vag_v1_deserialize,
    .stop = subghz_protocol_encoder_vag_v1_stop,
    .yield = subghz_protocol_encoder_vag_v1_yield,
};

const SubGhzProtocol subghz_protocol_vag_v1 = {
    .name = SUBGHZ_PROTOCOL_VAG_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_vag_v1_decoder,
    .encoder = &subghz_protocol_vag_v1_encoder,
};

static void vag_v1_fields(SubGhzProtocolDecoderVagV1* instance) {
    uint32_t uid = 0, state = 0;
    uint8_t button = 0;
    vag_v1_parse(instance->decoder.decode_data, &uid, &state, &button);
    instance->generic.serial = uid;
    instance->generic.cnt = state;
    instance->generic.btn = button;
    instance->generic.data = instance->decoder.decode_data;
    instance->generic.data_count_bit = VAG_V1_BITS;
}

void* subghz_protocol_decoder_vag_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderVagV1* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_vag_v1;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_VAG_V1_NAME;
    return instance;
}

void subghz_protocol_decoder_vag_v1_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_vag_v1_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderVagV1* instance = context;
    instance->decoder.parser_step = VagV1StepIdle;
    instance->frame = 0;
    instance->count = 0;
}

void subghz_protocol_decoder_vag_v1_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderVagV1* instance = context;

    switch(instance->decoder.parser_step) {
    case VagV1StepIdle:
        instance->decoder.parser_step =
            (level && vag_v1_is_start(duration)) ? VagV1StepStartLow : VagV1StepIdle;
        break;

    case VagV1StepStartLow:
        if(!level && vag_v1_is_start(duration)) {
            instance->frame = 0;
            instance->count = 0;
            instance->decoder.parser_step = VagV1StepBitHigh;
        } else {
            instance->decoder.parser_step =
                (level && vag_v1_is_start(duration)) ? VagV1StepStartLow : VagV1StepIdle;
        }
        break;

    case VagV1StepBitHigh:
        if(level && vag_v1_is_short(duration)) {
            instance->decoder.parser_step = VagV1StepBitLowShort;
        } else if(level && vag_v1_is_long(duration)) {
            instance->decoder.parser_step = VagV1StepBitLowLong;
        } else {
            instance->decoder.parser_step =
                (level && vag_v1_is_start(duration)) ? VagV1StepStartLow : VagV1StepIdle;
        }
        break;

    case VagV1StepBitLowShort:
    case VagV1StepBitLowLong: {
        bool high_was_long = (instance->decoder.parser_step == VagV1StepBitLowLong);
        bool bit_ok = false;
        bool bit_value = false;
        if(!level) {
            if(!high_was_long && vag_v1_is_long(duration)) {
                bit_ok = true;
                bit_value = false;
            } else if(high_was_long && vag_v1_is_short(duration)) {
                bit_ok = true;
                bit_value = true;
            }
        }
        if(!bit_ok) {
            instance->decoder.parser_step =
                (level && vag_v1_is_start(duration)) ? VagV1StepStartLow : VagV1StepIdle;
            break;
        }
        instance->frame |= (uint64_t)(bit_value ? 1u : 0u) << instance->count;
        instance->count++;
        instance->decoder.parser_step =
            (instance->count == VAG_V1_BITS) ? VagV1StepStop : VagV1StepBitHigh;
        break;
    }

    case VagV1StepStop:
        if(level && (vag_v1_is_long(duration) || vag_v1_is_start(duration))) {
            instance->decoder.parser_step = VagV1StepGap;
        } else {
            instance->decoder.parser_step =
                (level && vag_v1_is_start(duration)) ? VagV1StepStartLow : VagV1StepIdle;
        }
        break;

    case VagV1StepGap:
        if(!level && duration >= VAG_V1_GAP_MIN) {
            instance->decoder.parser_step = VagV1StepIdle;
            instance->decoder.decode_data = instance->frame;
            instance->decoder.decode_count_bit = VAG_V1_BITS;
            uint32_t uid, state;
            uint8_t button;
            if(vag_v1_parse(instance->frame, &uid, &state, &button)) {
                vag_v1_fields(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
        } else {
            instance->decoder.parser_step =
                (level && vag_v1_is_start(duration)) ? VagV1StepStartLow : VagV1StepIdle;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_vag_v1_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderVagV1* instance = context;
    return subghz_protocol_blocks_get_hash_data(&instance->decoder, (VAG_V1_BITS / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_vag_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderVagV1* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_vag_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderVagV1* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, VAG_V1_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = (uint8_t)VAG_V1_BITS;
    uint32_t uid, state;
    uint8_t button;
    if(!vag_v1_parse(instance->generic.data, &uid, &state, &button))
        return SubGhzProtocolStatusErrorParserOthers;
    vag_v1_fields(instance);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_vag_v1_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderVagV1* instance = context;
    subghz_custom_btn_set_original_if_unset(instance->generic.btn);
    subghz_custom_btn_set_max(2);
    subghz_block_generic_global_publish_btn(instance->generic.btn, 4);
    subghz_block_generic_global_publish_cnt(instance->generic.cnt, 24);
    const char* btn_name = (instance->generic.btn == VAG_V1_BTN_LOCK)   ? "Lock" :
                            (instance->generic.btn == VAG_V1_BTN_UNLOCK) ? "Unlock" :
                                                                           "?";
    /* "Sn:"/"Cnt:" (not "Uid:"/"State(LFSR):") on purpose: transmitter.c's
     * compact on-screen row (txv_after, "Sn:"/"Cnt:") only picks up these
     * exact tags - this is what makes the register's value show up on the
     * Transmitter screen itself, not just in this full-text detail view. */
    furi_string_cat_printf(
        output,
        "%s 60bit\r\n"
        "Key:%014llX\r\n"
        "Sn:%08lX Btn:%02X %s\r\n"
        "Cnt:%06lX\r\n"
        "No secret key - public LFSR obfuscation\r\n"
        "(Garcia et al., USENIX'16)\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        btn_name,
        (unsigned long)instance->generic.cnt);
}

/* --- Encoder --- */

static void vag_v1_emit(SubGhzProtocolEncoderVagV1* instance, bool level, uint32_t duration) {
    size_t size = instance->encoder.size_upload;
    LevelDuration* upload = instance->encoder.upload;
    if(size && level_duration_get_level(upload[size - 1]) == level) {
        upload[size - 1] =
            level_duration_make(level, duration + level_duration_get_duration(upload[size - 1]));
    } else if(size < VAG_V1_UPLOAD_CAPACITY) {
        upload[size] = level_duration_make(level, duration);
        instance->encoder.size_upload++;
    } else {
        instance->overflow = true;
    }
}

static void vag_v1_transmit(SubGhzProtocolEncoderVagV1* instance, uint64_t frame) {
    for(size_t copy = 0; copy < VAG_V1_COPIES; copy++) {
        vag_v1_emit(instance, true, 3u * VAG_V1_UNIT);
        vag_v1_emit(instance, false, 3u * VAG_V1_UNIT);
        for(size_t i = 0; i < VAG_V1_BITS; i++) {
            bool one = ((frame >> i) & 1u) == 1u;
            vag_v1_emit(instance, true, one ? 2u * VAG_V1_UNIT : VAG_V1_UNIT);
            vag_v1_emit(instance, false, one ? VAG_V1_UNIT : 2u * VAG_V1_UNIT);
        }
        vag_v1_emit(instance, true, 2u * VAG_V1_UNIT);
        vag_v1_emit(
            instance, false, (copy + 1 < VAG_V1_COPIES) ? VAG_V1_COPY_GAP : VAG_V1_FINAL_GAP);
    }
}

void* subghz_protocol_encoder_vag_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderVagV1* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_vag_v1;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_VAG_V1_NAME;
    instance->encoder.upload = malloc(VAG_V1_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    return instance;
}

void subghz_protocol_encoder_vag_v1_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderVagV1* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_vag_v1_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderVagV1* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_vag_v1_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderVagV1* instance = context;
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

SubGhzProtocolStatus
    subghz_protocol_encoder_vag_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderVagV1* instance = context;
    subghz_protocol_encoder_vag_v1_stop(instance);

    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, VAG_V1_BITS);
    if(status != SubGhzProtocolStatusOk) return status;

    uint32_t uid, state;
    uint8_t captured_button;
    if(!vag_v1_parse(instance->generic.data, &uid, &state, &captured_button))
        return SubGhzProtocolStatusErrorParserOthers;

    subghz_custom_btn_set_original_if_unset(captured_button);
    uint8_t button;
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        button = VAG_V1_BTN_LOCK;
        break;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        button = VAG_V1_BTN_UNLOCK;
        break;
    default:
        button = subghz_custom_btn_get_original();
        break;
    }
    if(button != VAG_V1_BTN_LOCK && button != VAG_V1_BTN_UNLOCK) button = captured_button;

    /* The register has no linear counter to override - "rolling counter
     * mult" instead picks how many 23-tick presses to advance by, so a user
     * who fell behind can resync by a few presses at once. Clamped: a
     * negative or huge value here would turn into a very long (or wrapped,
     * effectively infinite) tick loop below, not just a wrong counter. */
    int32_t mult = furi_hal_subghz_get_rolling_counter_mult();
    uint32_t presses = (mult < 1 || mult > 1000) ? 1u : (uint32_t)mult;
    uint32_t next_state = vag_v1_step(state, presses);
    uint64_t frame = vag_v1_build(uid, next_state, button);

    instance->encoder.size_upload = 0;
    instance->overflow = false;
    vag_v1_transmit(instance, frame);
    if(instance->overflow) return SubGhzProtocolStatusErrorEncoderGetUpload;

    uint32_t repeat = 1;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Repeat", &repeat, 1);
    instance->encoder.repeat = repeat;

    uint8_t key_data[sizeof(uint64_t)] = {0};
    for(size_t i = 0; i < sizeof(uint64_t); i++) {
        key_data[sizeof(uint64_t) - i - 1] = (frame >> (i * 8)) & 0xFF;
    }
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(flipper_format, "Key", key_data, sizeof(key_data)))
        return SubGhzProtocolStatusErrorParserOthers;
    instance->generic.data = frame;
    instance->generic.cnt = next_state;
    instance->generic.btn = button;
    subghz_block_generic_global.current_cnt = next_state;
    subghz_block_generic_global.current_btn = button;

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
