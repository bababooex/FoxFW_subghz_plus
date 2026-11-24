#include "opel_rolling.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include <lib/subghz/blocks/custom_btn_i.h>

#include <furi.h>
#include <stdlib.h>

/*
 * Opel Rolling: an old two-button Opel remote key (board marked
 * "28 5442 01"). KAT's own decoder (github.com/KaraZajac/KAT,
 * src/protocols/opel_rolling.rs), worked out from four real recordings
 * (399 presses read); no other reference reads this signal. Not our
 * opel_v0.c (PWM, "Unknown" encryption) or opel_v1.c (Manchester, XTEA).
 *
 * Decode + encode: the encoder is KAT's own "next press" rule (counter+1,
 * r+1, re-obfuscate, Lock/Unlock or repeat the captured button) - KAT
 * itself has not tried this against a real car either, so treat a resynced
 * remote with the usual caution. Still left out, as before: the separate
 * 32-bit "button held" frame format (a different, counter-less frame sent
 * every 40ms while the button stays down - this file only reads/sends the
 * main 60-bit press frame).
 *
 * Air format (AM, 433.89 MHz): the bit IS the length of a whole period (a
 * pause then a carrier pulse of the SAME length): 2 units (600us) = bit 0,
 * 3 units (900us) = bit 1, one unit = 150us. A press sends the frame 3
 * times, each copy opened by a sync (2 units carrier, 4 units pause, 4
 * units carrier) behind a run of 600us periods (57 of them before the
 * first copy, 4 before the other two).
 *
 * Frame (60 bits, 15 nibbles, MSB first): button nibble (0xA Unlock, 0xB
 * Lock), 24-bit serial, six nibbles of an obfuscated "plain" value (three
 * nibbles the recorded remote always sent as 0xACA, then its 12-bit press
 * counter) - each of those six nibbles is some remote-chosen nibble `r`
 * added to the plaintext, with some of the six then bit-inverted depending
 * on `r` (see FLAGS below), then `r` itself (inverted), then a rotating-
 * nibble XOR checksum (inverted) over the other 14 nibbles.
 */

#define OPEL_ROLLING_UNIT 150u
#define OPEL_ROLLING_TOL 60u
#define OPEL_ROLLING_FRAME_BITS 60u
#define OPEL_ROLLING_LEAD_MIN 2u

#define OPEL_ROLLING_UNLOCK 0xAu
#define OPEL_ROLLING_LOCK 0xBu

static inline bool opel_rolling_is_units(uint32_t duration, uint32_t units) {
    uint32_t ref = units * OPEL_ROLLING_UNIT;
    uint32_t diff = (duration > ref) ? (duration - ref) : (ref - duration);
    return diff <= OPEL_ROLLING_TOL;
}

/* For r = 0..15: which of the six plain nibbles are inverted behind the
 * addition (bit 5 = the first nibble). */
static const uint8_t opel_rolling_flags[16] = {
    0x15, 0x16, 0x19, 0x1A, 0x25, 0x26, 0x29, 0x2A,
    0x23, 0x2D, 0x27, 0x24, 0x3C, 0x1D, 0x1E, 0x35};

typedef enum {
    OpelRollingStepLead = 0,
    OpelRollingStepSyncHigh,
    OpelRollingStepLow,
    OpelRollingStepHighShort,
    OpelRollingStepHighLong,
} OpelRollingDecoderStep;

struct SubGhzProtocolDecoderOpelRolling {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint8_t highs; /* Lead state's count of 600us periods seen so far */
    uint64_t frame;
    uint8_t count;
};

static const char* const opel_rolling_names[2] = {"Unlock", "Lock"};

const SubGhzProtocolDecoder subghz_protocol_opel_rolling_decoder = {
    .alloc = subghz_protocol_decoder_opel_rolling_alloc,
    .free = subghz_protocol_decoder_opel_rolling_free,
    .reset = subghz_protocol_decoder_opel_rolling_reset,
    .feed = subghz_protocol_decoder_opel_rolling_feed,
    .get_hash_data = subghz_protocol_decoder_opel_rolling_get_hash_data,
    .serialize = subghz_protocol_decoder_opel_rolling_serialize,
    .deserialize = subghz_protocol_decoder_opel_rolling_deserialize,
    .get_string = subghz_protocol_decoder_opel_rolling_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_opel_rolling_encoder = {
    .alloc = subghz_protocol_encoder_opel_rolling_alloc,
    .free = subghz_protocol_encoder_opel_rolling_free,
    .deserialize = subghz_protocol_encoder_opel_rolling_deserialize,
    .stop = subghz_protocol_encoder_opel_rolling_stop,
    .yield = subghz_protocol_encoder_opel_rolling_yield,
};

const SubGhzProtocol subghz_protocol_opel_rolling = {
    .name = SUBGHZ_PROTOCOL_OPEL_ROLLING_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_opel_rolling_decoder,
    .encoder = &subghz_protocol_opel_rolling_encoder,
};

/* The check over the top `nibbles` nibbles of `value` (bit position
 * 4*nibbles-1 downto 0), highest first: rotate the accumulator left by one
 * bit within 4 bits, then XOR in the next nibble. */
static uint8_t opel_rolling_check(uint64_t value, int nibbles) {
    uint8_t acc = 0;
    for(int i = nibbles - 1; i >= 0; i--) {
        uint8_t nib = (uint8_t)((value >> (4 * i)) & 0xFu);
        acc = (uint8_t)((((acc << 1) | (acc >> 3)) & 0xFu) ^ nib);
    }
    return acc;
}

/* Validates the checksum and de-obfuscates the 6 plain nibbles of a 60-bit
 * frame. Returns false on a checksum mismatch. */
static bool opel_rolling_fields(SubGhzBlockGeneric* generic, uint64_t frame) {
    uint8_t computed = opel_rolling_check(frame >> 4, 14);
    if((computed ^ 0xFu) != (frame & 0xFu)) return false;

    uint8_t r = (uint8_t)((~(frame >> 4)) & 0xFu);
    uint32_t plain = 0;
    for(int i = 0; i < 6; i++) {
        uint8_t n = (uint8_t)((frame >> (28 - 4 * i)) & 0xFu);
        bool inverted = ((opel_rolling_flags[r] >> (5 - i)) & 1u) != 0;
        uint8_t v = inverted ? (uint8_t)(n ^ 0xFu) : n;
        plain = (plain << 4) | (uint32_t)((v - r) & 0xFu);
    }

    generic->data = frame;
    generic->data_count_bit = OPEL_ROLLING_FRAME_BITS;
    generic->btn = (uint8_t)((frame >> 56) & 0xFu);
    generic->serial = (uint32_t)((frame >> 32) & 0xFFFFFFu);
    generic->cnt = plain & 0xFFFu;
    generic->seed = plain; /* the full 24-bit plain value (3 unknown nibbles + the counter) */
    return true;
}

/* The nibble at frame position 4 (just above the check nibble), inverted -
 * the same "r" opel_rolling_fields() above extracts to de-obfuscate. */
static inline uint8_t opel_rolling_r_of(uint64_t frame) {
    return (uint8_t)((~(frame >> 4)) & 0xFu);
}

/* The inverse of opel_rolling_fields()'s de-obfuscation: rebuilds the 60
 * frame bits (including the check nibble) from button/serial/plain/r. Same
 * construction as KAT's own Fields::frame(). */
static uint64_t opel_rolling_build(uint8_t btn, uint32_t serial, uint32_t plain, uint8_t r) {
    uint64_t frame = ((uint64_t)(btn & 0xFu) << 24) | (uint64_t)(serial & 0xFFFFFFu);
    for(int i = 0; i < 6; i++) {
        uint8_t v = (uint8_t)((((plain >> (20 - 4 * i)) & 0xFu) + r) & 0xFu);
        bool inverted = ((opel_rolling_flags[r & 0xFu] >> (5 - i)) & 1u) != 0;
        uint8_t n = inverted ? (uint8_t)(v ^ 0xFu) : v;
        frame = (frame << 4) | (uint64_t)n;
    }
    frame = (frame << 4) | (uint64_t)((~r) & 0xFu);
    uint8_t chk = opel_rolling_check(frame, 14);
    return (frame << 4) | (uint64_t)(chk ^ 0xFu);
}

void* subghz_protocol_decoder_opel_rolling_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderOpelRolling* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_opel_rolling;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_OPEL_ROLLING_NAME;
    return instance;
}

void subghz_protocol_decoder_opel_rolling_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_opel_rolling_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderOpelRolling* instance = context;
    instance->decoder.parser_step = OpelRollingStepLead;
    instance->highs = 0;
}

/* A pulse that fits nowhere: start over, taking this pulse itself as the
 * first lead carrier when it qualifies. */
static void
    opel_rolling_restart(SubGhzProtocolDecoderOpelRolling* instance, bool level, uint32_t duration) {
    instance->decoder.parser_step = OpelRollingStepLead;
    instance->highs = (uint8_t)((level && opel_rolling_is_units(duration, 2)) ? 1 : 0);
}

void subghz_protocol_decoder_opel_rolling_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderOpelRolling* instance = context;

    switch(instance->decoder.parser_step) {
    case OpelRollingStepLead:
        if(level && opel_rolling_is_units(duration, 2)) {
            if(instance->highs < 0xFF) instance->highs++;
        } else if(!level && opel_rolling_is_units(duration, 2) && instance->highs > 0) {
            /* a lead period's own pause - stay */
        } else if(
            !level && instance->highs >= OPEL_ROLLING_LEAD_MIN &&
            opel_rolling_is_units(duration, 4)) {
            instance->decoder.parser_step = OpelRollingStepSyncHigh;
        } else {
            opel_rolling_restart(instance, level, duration);
        }
        break;

    case OpelRollingStepSyncHigh:
        if(level && opel_rolling_is_units(duration, 4)) {
            instance->frame = 0;
            instance->count = 0;
            instance->decoder.parser_step = OpelRollingStepLow;
        } else {
            opel_rolling_restart(instance, level, duration);
        }
        break;

    case OpelRollingStepLow:
        if(!level && opel_rolling_is_units(duration, 2)) {
            instance->decoder.parser_step = OpelRollingStepHighShort;
        } else if(!level && opel_rolling_is_units(duration, 3)) {
            instance->decoder.parser_step = OpelRollingStepHighLong;
        } else {
            opel_rolling_restart(instance, level, duration);
        }
        break;

    case OpelRollingStepHighShort:
    case OpelRollingStepHighLong: {
        bool want_long = (instance->decoder.parser_step == OpelRollingStepHighLong);
        if(!level || !opel_rolling_is_units(duration, want_long ? 3 : 2)) {
            opel_rolling_restart(instance, level, duration);
            break;
        }
        instance->frame = (instance->frame << 1) | (want_long ? 1u : 0u);
        instance->count++;
        if(instance->count == OPEL_ROLLING_FRAME_BITS) {
            instance->decoder.parser_step = OpelRollingStepLead;
            instance->highs = 0;
            if(opel_rolling_fields(&instance->generic, instance->frame)) {
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
        } else {
            instance->decoder.parser_step = OpelRollingStepLow;
        }
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_opel_rolling_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderOpelRolling* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_opel_rolling_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderOpelRolling* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_opel_rolling_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderOpelRolling* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, OPEL_ROLLING_FRAME_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    if(!opel_rolling_fields(&instance->generic, instance->generic.data))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_opel_rolling_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderOpelRolling* instance = context;
    subghz_custom_btn_set_original_if_unset(instance->generic.btn);
    subghz_custom_btn_set_max(2);
    subghz_block_generic_global_publish_btn(instance->generic.btn, 4);
    subghz_block_generic_global_publish_cnt(instance->generic.cnt, 12);
    uint8_t btn = instance->generic.btn;
    const char* name = (btn == OPEL_ROLLING_UNLOCK) ? opel_rolling_names[0] :
                       (btn == OPEL_ROLLING_LOCK)   ? opel_rolling_names[1] :
                                                       "?";
    furi_string_cat_printf(
        output,
        "%s 60bit\r\n"
        "Key:%015llX\r\n"
        "Sn:%06lX Btn:%01X %s\r\n"
        "Cnt:%03lX\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->generic.serial,
        btn,
        name,
        (unsigned long)instance->generic.cnt);
}

/* --- Encoder --- */

#define OPEL_ROLLING_COPIES 3u
#define OPEL_ROLLING_LEAD_FIRST 57u
#define OPEL_ROLLING_LEAD_NEXT 4u
#define OPEL_ROLLING_COPY_GAP 1200u
#define OPEL_ROLLING_FINAL_GAP 10000u
#define OPEL_ROLLING_UPLOAD_CAPACITY \
    (OPEL_ROLLING_COPIES * (2u * OPEL_ROLLING_LEAD_FIRST + 3u + 2u * OPEL_ROLLING_FRAME_BITS + 1u))

struct SubGhzProtocolEncoderOpelRolling {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    bool overflow;
};

static void
    opel_rolling_emit(SubGhzProtocolEncoderOpelRolling* instance, bool level, uint32_t duration) {
    size_t size = instance->encoder.size_upload;
    LevelDuration* upload = instance->encoder.upload;
    if(size && level_duration_get_level(upload[size - 1]) == level) {
        upload[size - 1] =
            level_duration_make(level, duration + level_duration_get_duration(upload[size - 1]));
    } else if(size < OPEL_ROLLING_UPLOAD_CAPACITY) {
        upload[size] = level_duration_make(level, duration);
        instance->encoder.size_upload++;
    } else {
        instance->overflow = true;
    }
}

/* One press: the frame OPEL_ROLLING_COPIES times, each behind its own lead
 * run + sync (57 periods for the first copy, 4 for the other two, same as
 * a real remote and as KAT's own upload()). */
static void opel_rolling_transmit(SubGhzProtocolEncoderOpelRolling* instance, uint64_t frame) {
    for(uint32_t copy = 0; copy < OPEL_ROLLING_COPIES; copy++) {
        uint32_t lead = (copy == 0) ? OPEL_ROLLING_LEAD_FIRST : OPEL_ROLLING_LEAD_NEXT;
        for(uint32_t i = 0; i < lead; i++) {
            opel_rolling_emit(instance, true, 2u * OPEL_ROLLING_UNIT);
            opel_rolling_emit(instance, false, 2u * OPEL_ROLLING_UNIT);
        }
        opel_rolling_emit(instance, true, 2u * OPEL_ROLLING_UNIT);
        opel_rolling_emit(instance, false, 4u * OPEL_ROLLING_UNIT);
        opel_rolling_emit(instance, true, 4u * OPEL_ROLLING_UNIT);
        for(int i = OPEL_ROLLING_FRAME_BITS - 1; i >= 0; i--) {
            bool one = ((frame >> i) & 1u) != 0;
            uint32_t half = (one ? 3u : 2u) * OPEL_ROLLING_UNIT;
            opel_rolling_emit(instance, false, half);
            opel_rolling_emit(instance, true, half);
        }
        opel_rolling_emit(
            instance,
            false,
            (copy + 1 < OPEL_ROLLING_COPIES) ? OPEL_ROLLING_COPY_GAP : OPEL_ROLLING_FINAL_GAP);
    }
}

void* subghz_protocol_encoder_opel_rolling_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderOpelRolling* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_opel_rolling;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_OPEL_ROLLING_NAME;
    instance->encoder.upload = malloc(OPEL_ROLLING_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    return instance;
}

void subghz_protocol_encoder_opel_rolling_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderOpelRolling* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_opel_rolling_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderOpelRolling* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_opel_rolling_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderOpelRolling* instance = context;
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
    subghz_protocol_encoder_opel_rolling_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderOpelRolling* instance = context;
    subghz_protocol_encoder_opel_rolling_stop(instance);

    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, OPEL_ROLLING_FRAME_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    if(!opel_rolling_fields(&instance->generic, instance->generic.data))
        return SubGhzProtocolStatusErrorParserOthers;

    uint8_t captured_button = instance->generic.btn;
    subghz_custom_btn_set_original_if_unset(captured_button);
    uint8_t button;
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        button = OPEL_ROLLING_LOCK;
        break;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        button = OPEL_ROLLING_UNLOCK;
        break;
    default:
        button = subghz_custom_btn_get_original();
        break;
    }
    if(button != OPEL_ROLLING_LOCK && button != OPEL_ROLLING_UNLOCK) button = captured_button;

    /* "Next press": the counter (the low 12 bits of the captured plain
     * value) advances, the upper 12 bits (the unknown "ACA" prefix) stay,
     * and r advances too - same rule as KAT's own Fields::next(), applied
     * however many presses the rolling-counter-mult setting asks for. */
    int32_t mult = furi_hal_subghz_get_rolling_counter_mult();
    uint32_t presses = (mult < 1 || mult > 4096) ? 1u : (uint32_t)mult;
    uint32_t plain = instance->generic.seed;
    uint8_t r = opel_rolling_r_of(instance->generic.data);
    for(uint32_t p = 0; p < presses; p++) {
        plain = (plain & 0xFFF000u) | ((plain + 1u) & 0xFFFu);
        r = (uint8_t)((r + 1u) & 0xFu);
    }
    uint64_t frame =
        opel_rolling_build(button, instance->generic.serial, plain, r);

    instance->encoder.size_upload = 0;
    instance->overflow = false;
    opel_rolling_transmit(instance, frame);
    if(instance->overflow) return SubGhzProtocolStatusErrorEncoderGetUpload;

    uint32_t repeat = 1;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Repeat", &repeat, 1);
    instance->encoder.repeat = repeat;

    /* Persist the advanced frame back into "Key" - so a second press in the
     * same Transmitter session (deserialize called again without the file
     * being reloaded from disk) continues from here, not from the frame
     * that was originally captured. Same pattern as vag_v1.c. */
    uint8_t key_data[sizeof(uint64_t)] = {0};
    for(size_t i = 0; i < sizeof(uint64_t); i++) {
        key_data[sizeof(uint64_t) - i - 1] = (uint8_t)((frame >> (i * 8)) & 0xFFu);
    }
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(flipper_format, "Key", key_data, sizeof(key_data)))
        return SubGhzProtocolStatusErrorParserOthers;

    opel_rolling_fields(&instance->generic, frame);
    subghz_block_generic_global.current_cnt = instance->generic.cnt;
    subghz_block_generic_global.current_btn = instance->generic.btn;

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
