#include "proteco.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/subghz/blocks/custom_btn_i.h>

#include <furi.h>
#include <stdlib.h>

/*
 * Proteco MOVER gate remotes - 24-bit fixed code (22-bit serial, 2-bit
 * button). Ported from RogueMaster's lib/subghz/protocols/proteco.c (another
 * Flipper firmware fork; RogueMaster registers it, this tree didn't have
 * it), cross-checked against the KAT project's independent re-derivation
 * (github.com/KaraZajac/KAT, src/protocols/proteco.rs). Both agree on the
 * timing, frame layout and button table below; RogueMaster's own unit-test
 * captures are real recordings of one TX3 remote's buttons A/B/C.
 *
 * Air format (AM, 433.92 MHz): CAME-style line coding at Te ~440us (CAME
 * proper runs at ~320us - this remote's own oscillator is just slower). A
 * frame: a guard LOW of ~20.5*Te (~9.0 ms), a start HIGH of one Te, then 24
 * cells MSB-first, each a LOW then a HIGH: LOW 2*Te + HIGH Te = bit 1, LOW
 * Te + HIGH 2*Te = bit 0. The frame repeats while the button is held - the
 * LOW that closes one frame (checked against the frame's own measured Te,
 * 18.5..22.5 Te) is simultaneously the next frame's guard, so a held button
 * is read as one guard followed by back-to-back frames with no extra gap
 * between them.
 *
 * No checksum, no counter - this is a fixed code.
 */

#define PROTECO_TE_SHORT     440u
#define PROTECO_TE_LONG      880u
#define PROTECO_TE_DELTA     150u
#define PROTECO_BITS         24u
#define PROTECO_GUARD_MIN    (PROTECO_TE_SHORT * 4u)
#define PROTECO_GUARD_TE_X2_MIN 37u
#define PROTECO_GUARD_TE_X2_MAX 45u
#define PROTECO_BTN_MASK     0x3u
#define PROTECO_TX_REPEAT    10u
#define PROTECO_FRAME_PULSES (PROTECO_BITS * 2u + 2u)
#define PROTECO_UPLOAD_CAPACITY (PROTECO_FRAME_PULSES * PROTECO_TX_REPEAT)

#define PROTECO_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < PROTECO_TE_DELTA)

/* subghz_protocol_proteco_btn: raw low 2 bits -> button 1..4 (so 0 keeps its
 * "no button seen yet" meaning). Raw 2 = A (the gate), raw 0 = B, raw 1 = C -
 * a TX3's real buttons; raw 3 = button 4, unverified (no sample seen). */
static const uint8_t proteco_btn[4] = {2, 3, 1, 4};
/* The inverse: button 1..4 -> raw low 2 bits. */
static const uint8_t proteco_btn_raw[5] = {0, 2, 0, 1, 3};

typedef enum {
    ProtecoStepReset = 0,
    ProtecoStepFoundStartBit,
    ProtecoStepSaveDuration,
    ProtecoStepCheckDuration,
} ProtecoDecoderStep;

struct SubGhzProtocolDecoderProteco {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint32_t te_sum;
    uint32_t guard;
};

struct SubGhzProtocolEncoderProteco {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    bool overflow;
};

static void proteco_fields(SubGhzBlockGeneric* generic, uint64_t data) {
    data &= 0xFFFFFFu;
    generic->data = data;
    generic->data_count_bit = PROTECO_BITS;
    generic->serial = (uint32_t)((data >> 2) & 0x3FFFFFu);
    generic->btn = proteco_btn[data & PROTECO_BTN_MASK];
}

/* The code sent for a given button: 1..=4 rewrites the low 2 bits, anything
 * else (0, or an unknown value) keeps the captured code unchanged. */
static uint64_t proteco_with_button(uint64_t data, uint8_t button) {
    if(button >= 1 && button <= 4) {
        return (data & ~(uint64_t)PROTECO_BTN_MASK) | proteco_btn_raw[button];
    }
    return data;
}

const SubGhzProtocolDecoder subghz_protocol_proteco_decoder = {
    .alloc = subghz_protocol_decoder_proteco_alloc,
    .free = subghz_protocol_decoder_proteco_free,
    .reset = subghz_protocol_decoder_proteco_reset,
    .feed = subghz_protocol_decoder_proteco_feed,
    .get_hash_data = subghz_protocol_decoder_proteco_get_hash_data,
    .serialize = subghz_protocol_decoder_proteco_serialize,
    .deserialize = subghz_protocol_decoder_proteco_deserialize,
    .get_string = subghz_protocol_decoder_proteco_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_proteco_encoder = {
    .alloc = subghz_protocol_encoder_proteco_alloc,
    .free = subghz_protocol_encoder_proteco_free,
    .deserialize = subghz_protocol_encoder_proteco_deserialize,
    .stop = subghz_protocol_encoder_proteco_stop,
    .yield = subghz_protocol_encoder_proteco_yield,
};

const SubGhzProtocol subghz_protocol_proteco = {
    .name = SUBGHZ_PROTOCOL_PROTECO_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_proteco_decoder,
    .encoder = &subghz_protocol_proteco_encoder,
};

void* subghz_protocol_decoder_proteco_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderProteco* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_proteco;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_PROTECO_NAME;
    return instance;
}

void subghz_protocol_decoder_proteco_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_proteco_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderProteco* instance = context;
    instance->decoder.parser_step = ProtecoStepReset;
}

/* Is the LOW that opened this frame ~20.5 Te of the frame's own clock (the
 * mean Te over the start bit + 24 cells, i.e. te_sum / (24*3 + 1))? Matches
 * both a remote slightly off the nominal 440us and keeps out an unrelated
 * ~9ms low that merely sits near a 24-bit frame by coincidence. */
static bool proteco_guard_ok(const SubGhzProtocolDecoderProteco* instance) {
    uint32_t te = instance->te_sum / (PROTECO_BITS * 3u + 1u);
    uint32_t lo = te * PROTECO_GUARD_TE_X2_MIN / 2u;
    uint32_t hi = te * PROTECO_GUARD_TE_X2_MAX / 2u;
    return instance->guard >= lo && instance->guard <= hi;
}

void subghz_protocol_decoder_proteco_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderProteco* instance = context;

    switch(instance->decoder.parser_step) {
    case ProtecoStepReset:
        if(!level && duration >= PROTECO_GUARD_MIN) {
            instance->guard = duration;
            instance->decoder.parser_step = ProtecoStepFoundStartBit;
        }
        break;

    case ProtecoStepFoundStartBit:
        if(level && PROTECO_IN_TOL(duration, PROTECO_TE_SHORT)) {
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->te_sum = duration;
            instance->decoder.parser_step = ProtecoStepSaveDuration;
        } else {
            instance->decoder.parser_step = ProtecoStepReset;
        }
        break;

    /* A cell opens on the LOW - unless this LOW is long enough to be the
     * guard that closes the frame (and opens the next one). */
    case ProtecoStepSaveDuration:
        if(level) {
            instance->decoder.parser_step = ProtecoStepReset;
            break;
        }
        if(duration >= PROTECO_GUARD_MIN) {
            bool found = (instance->decoder.decode_count_bit == PROTECO_BITS) &&
                         proteco_guard_ok(instance);
            instance->guard = duration;
            instance->decoder.parser_step = ProtecoStepFoundStartBit;
            if(found) {
                proteco_fields(&instance->generic, instance->decoder.decode_data);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            break;
        }
        instance->decoder.te_last = duration;
        instance->te_sum += duration;
        instance->decoder.parser_step = ProtecoStepCheckDuration;
        break;

    /* ...and closes on the HIGH. */
    case ProtecoStepCheckDuration:
        if(!level) {
            instance->decoder.parser_step = ProtecoStepReset;
            break;
        }
        if(PROTECO_IN_TOL(instance->decoder.te_last, PROTECO_TE_SHORT) &&
           PROTECO_IN_TOL(duration, PROTECO_TE_LONG)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 0);
        } else if(
            PROTECO_IN_TOL(instance->decoder.te_last, PROTECO_TE_LONG) &&
            PROTECO_IN_TOL(duration, PROTECO_TE_SHORT)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 1);
        } else {
            instance->decoder.parser_step = ProtecoStepReset;
            break;
        }
        instance->te_sum += duration;
        instance->decoder.parser_step = ProtecoStepSaveDuration;
        break;
    }
}

uint8_t subghz_protocol_decoder_proteco_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderProteco* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_proteco_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderProteco* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_proteco_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderProteco* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, PROTECO_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    proteco_fields(&instance->generic, instance->generic.data);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_proteco_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderProteco* instance = context;
    subghz_custom_btn_set_original_if_unset(instance->generic.btn);
    subghz_custom_btn_set_max(4);
    subghz_block_generic_global_publish_btn(instance->generic.btn, 2);
    furi_string_cat_printf(
        output,
        "%s 24bit\r\n"
        "Key:%06llX\r\n"
        "Sn:%06lX Btn:%01X\r\n"
        "Fixed code - no counter\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->generic.serial,
        instance->generic.btn);
}

/* --- Encoder --- */

static void proteco_emit(SubGhzProtocolEncoderProteco* instance, bool level, uint32_t duration) {
    size_t size = instance->encoder.size_upload;
    LevelDuration* upload = instance->encoder.upload;
    if(size && level_duration_get_level(upload[size - 1]) == level) {
        upload[size - 1] =
            level_duration_make(level, duration + level_duration_get_duration(upload[size - 1]));
    } else if(size < PROTECO_UPLOAD_CAPACITY) {
        upload[size] = level_duration_make(level, duration);
        instance->encoder.size_upload++;
    } else {
        instance->overflow = true;
    }
}

/* One frame: guard, start bit, 24 cells MSB-first. The guard doubles as the
 * previous copy's closing gap, so TX_REPEAT copies of just this is the full
 * held-button transmission - no extra gap needed between copies. */
static void proteco_build_frame(SubGhzProtocolEncoderProteco* instance, uint64_t data) {
    proteco_emit(instance, false, PROTECO_TE_SHORT * 41u / 2u);
    proteco_emit(instance, true, PROTECO_TE_SHORT);
    for(int i = PROTECO_BITS - 1; i >= 0; i--) {
        bool one = ((data >> i) & 1u) != 0;
        proteco_emit(instance, false, one ? PROTECO_TE_LONG : PROTECO_TE_SHORT);
        proteco_emit(instance, true, one ? PROTECO_TE_SHORT : PROTECO_TE_LONG);
    }
}

void* subghz_protocol_encoder_proteco_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderProteco* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_proteco;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_PROTECO_NAME;
    instance->encoder.upload = malloc(PROTECO_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    return instance;
}

void subghz_protocol_encoder_proteco_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderProteco* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_proteco_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderProteco* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_proteco_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderProteco* instance = context;
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
    subghz_protocol_encoder_proteco_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderProteco* instance = context;
    subghz_protocol_encoder_proteco_stop(instance);

    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, PROTECO_BITS);
    if(status != SubGhzProtocolStatusOk) return status;

    uint8_t captured_button = proteco_btn[instance->generic.data & PROTECO_BTN_MASK];
    subghz_custom_btn_set_original_if_unset(captured_button);
    uint8_t button;
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        button = 1; /* A */
        break;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        button = 2; /* B */
        break;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        button = 3; /* C */
        break;
    case SUBGHZ_CUSTOM_BTN_RIGHT:
        button = 4; /* unverified 4th code */
        break;
    default:
        button = subghz_custom_btn_get_original();
        break;
    }
    uint64_t data = proteco_with_button(instance->generic.data, button);

    instance->encoder.size_upload = 0;
    instance->overflow = false;
    for(uint32_t copy = 0; copy < PROTECO_TX_REPEAT; copy++) {
        proteco_build_frame(instance, data);
    }
    if(instance->overflow) return SubGhzProtocolStatusErrorEncoderGetUpload;

    uint32_t repeat = 1;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Repeat", &repeat, 1);
    instance->encoder.repeat = repeat;

    proteco_fields(&instance->generic, data);
    subghz_block_generic_global.current_btn = instance->generic.btn;

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
