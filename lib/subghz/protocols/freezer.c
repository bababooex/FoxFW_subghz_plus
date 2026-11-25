#include "freezer.h"
<<<<<<< HEAD
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"

#include <furi.h>
#include <stdlib.h>

/*
 * Freezer - 24-bit fixed code (20-bit serial, 4-bit button). Ported from
 * RogueMaster's lib/subghz/protocols/freezer.c (no other reference has it),
 * cross-checked against the KAT project's independent re-derivation
 * (github.com/KaraZajac/KAT, src/protocols/freezer.rs).
 *
 * Air format (AM, 433.92 MHz): a carrier-off gap of at least 30*te_short
 * (7500us) opens a frame, then a lone short HIGH that belongs to no cell,
 * then 24 cells MSB-first, each a LOW then a HIGH: short LOW + long HIGH =
 * bit 1, long LOW + short HIGH = bit 0 (nominal short 250us, long 500us).
 * The encoder closes each frame with a 9000us (36*te_short) gap.
 *
 * No checksum - RogueMaster's own decoder instead requires the SAME 24-bit
 * frame to repeat once before reporting it (a remote repeats its frame for
 * as long as the button is held; a stray run of plausible noise bits does
 * not repeat identically). This file keeps that requirement: the first copy
 * of a press is never reported, only the second and later ones.
 */

#define FREEZER_TE_SHORT  250u
#define FREEZER_TE_LONG   500u
#define FREEZER_BITS      24u
/* The C's own split classification, not a window around te_short/te_long:
 * "the measured classes are only 33us apart while each spreads over 200us".
 * Anything over SYMBOL_MAX (700us) is refused on purpose, to keep Princeton
 * frames (long symbol ~1170us) from being misread as Freezer. */
#define FREEZER_SYMBOL_MIN   120u
#define FREEZER_SYMBOL_SPLIT 375u
#define FREEZER_SYMBOL_MAX   700u
#define FREEZER_GAP_MIN      (FREEZER_TE_SHORT * 30u)
#define FREEZER_GAP_DURATION (FREEZER_TE_SHORT * 36u)
#define FREEZER_TX_REPEAT 3u
#define FREEZER_FRAME_PULSES (FREEZER_BITS * 2u + 2u)
#define FREEZER_UPLOAD_CAPACITY (FREEZER_FRAME_PULSES * FREEZER_TX_REPEAT)

static inline bool freezer_is_short(uint32_t d) {
    return d >= FREEZER_SYMBOL_MIN && d <= FREEZER_SYMBOL_SPLIT;
}
static inline bool freezer_is_long(uint32_t d) {
    return d > FREEZER_SYMBOL_SPLIT && d <= FREEZER_SYMBOL_MAX;
}

typedef enum {
    FreezerStepReset = 0,
    FreezerStepStart,
    FreezerStepSaveDuration,
    FreezerStepCheckDuration,
} FreezerDecoderStep;

struct SubGhzProtocolDecoderFreezer {
    SubGhzProtocolDecoderBase base;
=======
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "common.h"
#include "../encode_plugin.h"
#include "decode_plugin.h"

#define TAG "SubGhzProtocolFreezer"

static const SubGhzBlockConst subghz_protocol_freezer_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 120,
    .min_count_bit_for_found = 24,
};

#define FREEZER_SYMBOL_MIN   120
#define FREEZER_SYMBOL_SPLIT 375
#define FREEZER_SYMBOL_MAX   700

#define FREEZER_GAP_MIN_TE   30
#define FREEZER_GAP_MIN      ((uint32_t)subghz_protocol_freezer_const.te_short * FREEZER_GAP_MIN_TE)
#define FREEZER_GAP_DURATION ((uint32_t)subghz_protocol_freezer_const.te_short * 36)

struct SubGhzProtocolDecoderFreezer {
    SubGhzProtocolDecoderBase base;

>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint64_t last_data;
};
<<<<<<< HEAD

struct SubGhzProtocolEncoderFreezer {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    bool overflow;
};

static void freezer_fields(SubGhzBlockGeneric* generic, uint64_t data) {
    data &= 0xFFFFFFu;
    generic->data = data;
    generic->data_count_bit = FREEZER_BITS;
    generic->serial = (uint32_t)((data >> 4) & 0xFFFFFu);
    generic->btn = (uint8_t)(data & 0xFu);
}

const SubGhzProtocolDecoder subghz_protocol_freezer_decoder = {
    .alloc = subghz_protocol_decoder_freezer_alloc,
    .free = subghz_protocol_decoder_freezer_free,
    .reset = subghz_protocol_decoder_freezer_reset,
    .feed = subghz_protocol_decoder_freezer_feed,
    .get_hash_data = subghz_protocol_decoder_freezer_get_hash_data,
    .serialize = subghz_protocol_decoder_freezer_serialize,
=======
SUBGHZ_ASSERT_DECODER_COMMON_LAYOUT(SubGhzProtocolDecoderFreezer);

struct SubGhzProtocolEncoderFreezer {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};
SUBGHZ_ASSERT_ENCODER_GENERIC_LAYOUT(SubGhzProtocolEncoderFreezer);

typedef enum {
    FreezerDecoderStepReset = 0,
    FreezerDecoderStepStart,
    FreezerDecoderStepSaveDuration,
    FreezerDecoderStepCheckDuration,
} FreezerDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_freezer_decoder = {
    .alloc = subghz_protocol_decoder_freezer_alloc,
    .free = subghz_protocol_decoder_common_free,

    .feed = subghz_protocol_decoder_freezer_feed,
    .reset = subghz_protocol_decoder_freezer_reset,

    .get_hash_data = subghz_protocol_decoder_common_get_hash_data,
    .serialize = subghz_protocol_decoder_common_serialize,
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
    .deserialize = subghz_protocol_decoder_freezer_deserialize,
    .get_string = subghz_protocol_decoder_freezer_get_string,
};

<<<<<<< HEAD
const SubGhzProtocolEncoder subghz_protocol_freezer_encoder = {
    .alloc = subghz_protocol_encoder_freezer_alloc,
    .free = subghz_protocol_encoder_freezer_free,
    .deserialize = subghz_protocol_encoder_freezer_deserialize,
    .stop = subghz_protocol_encoder_freezer_stop,
    .yield = subghz_protocol_encoder_freezer_yield,
};
=======
static const SubGhzEncoderExternal subghz_protocol_freezer_encoder_ext =
    SUBGHZ_ENCODER_EXTERNAL("enc_freezer");
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5

const SubGhzProtocol subghz_protocol_freezer = {
    .name = SUBGHZ_PROTOCOL_FREEZER_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
<<<<<<< HEAD
    .decoder = &subghz_protocol_freezer_decoder,
    .encoder = &subghz_protocol_freezer_encoder,
};

void* subghz_protocol_decoder_freezer_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFreezer* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_freezer;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_FREEZER_NAME;
    return instance;
}

void subghz_protocol_decoder_freezer_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_freezer_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFreezer* instance = context;
    instance->decoder.parser_step = FreezerStepReset;
    instance->last_data = 0;
}

void subghz_protocol_decoder_freezer_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderFreezer* instance = context;

    switch(instance->decoder.parser_step) {
    case FreezerStepReset:
        if(!level && duration >= FREEZER_GAP_MIN) {
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = FreezerStepStart;
        }
        break;

    case FreezerStepStart:
        instance->decoder.parser_step =
            (level && freezer_is_short(duration)) ? FreezerStepSaveDuration : FreezerStepReset;
        break;

    /* A cell opens on the LOW - unless this LOW is the gap that closes the
     * frame (and, on this protocol, opens the next one too). */
    case FreezerStepSaveDuration:
        if(level) {
            instance->decoder.parser_step = FreezerStepReset;
            break;
        }
        if(duration >= FREEZER_GAP_MIN) {
            bool full = (instance->decoder.decode_count_bit == FREEZER_BITS);
            bool found = full && instance->last_data != 0 &&
                         instance->decoder.decode_data == instance->last_data;
            if(found) freezer_fields(&instance->generic, instance->decoder.decode_data);
            instance->last_data = full ? instance->decoder.decode_data : 0;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = FreezerStepStart;
            if(found && instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
            break;
        }
        instance->decoder.te_last = duration;
        instance->decoder.parser_step = FreezerStepCheckDuration;
        break;

    /* ...and closes on the HIGH. */
    case FreezerStepCheckDuration:
        if(!level) {
            instance->decoder.parser_step = FreezerStepReset;
            break;
        }
        if(freezer_is_short(instance->decoder.te_last) && freezer_is_long(duration)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 1);
            instance->decoder.parser_step = FreezerStepSaveDuration;
        } else if(
            freezer_is_long(instance->decoder.te_last) && freezer_is_short(duration)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 0);
            instance->decoder.parser_step = FreezerStepSaveDuration;
        } else {
            /* A broken cell clears the repeat memory too - a stray run of
             * plausible-looking noise should not get a second chance to
             * match a now-stale last_data. */
            instance->last_data = 0;
            instance->decoder.parser_step = FreezerStepReset;
=======

    .decoder = &subghz_protocol_freezer_decoder,
    .encoder = &subghz_protocol_freezer_encoder_ext.vtable,
};

static void subghz_protocol_freezer_check_remote_controller(SubGhzBlockGeneric* instance) {
    instance->serial = (instance->data >> 4) & 0xFFFFF;
    instance->btn = instance->data & 0xF;
}

void* subghz_protocol_decoder_freezer_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFreezer* instance = subghz_protocol_decoder_common_alloc(
        sizeof(SubGhzProtocolDecoderFreezer), &subghz_protocol_freezer);
    instance->last_data = 0;

    return instance;
}

void subghz_protocol_decoder_freezer_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFreezer* instance = context;
    instance->decoder.parser_step = FreezerDecoderStepReset;
    instance->last_data = 0;
}

static bool subghz_protocol_freezer_is_short(uint32_t duration) {
    return (duration >= FREEZER_SYMBOL_MIN) && (duration <= FREEZER_SYMBOL_SPLIT);
}

static bool subghz_protocol_freezer_is_long(uint32_t duration) {
    return (duration > FREEZER_SYMBOL_SPLIT) && (duration <= FREEZER_SYMBOL_MAX);
}

void subghz_protocol_decoder_freezer_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderFreezer* instance = context;

    switch(instance->decoder.parser_step) {
    case FreezerDecoderStepReset:
        if((!level) && (duration >= FREEZER_GAP_MIN)) {
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = FreezerDecoderStepStart;
        }
        break;

    case FreezerDecoderStepStart:
        if(level && subghz_protocol_freezer_is_short(duration)) {
            instance->decoder.parser_step = FreezerDecoderStepSaveDuration;
        } else {
            instance->decoder.parser_step = FreezerDecoderStepReset;
        }
        break;

    case FreezerDecoderStepSaveDuration:
        if(level) {
            instance->decoder.parser_step = FreezerDecoderStepReset;
            break;
        }
        if(duration >= FREEZER_GAP_MIN) {

            if((instance->decoder.decode_count_bit ==
                subghz_protocol_freezer_const.min_count_bit_for_found) &&
               (instance->decoder.decode_data == instance->last_data) && instance->last_data) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;

                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            if(instance->decoder.decode_count_bit ==
               subghz_protocol_freezer_const.min_count_bit_for_found) {
                instance->last_data = instance->decoder.decode_data;
            } else {
                instance->last_data = 0;
            }
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = FreezerDecoderStepStart;
            break;
        }
        instance->decoder.te_last = duration;
        instance->decoder.parser_step = FreezerDecoderStepCheckDuration;
        break;

    case FreezerDecoderStepCheckDuration:
        if(!level) {
            instance->decoder.parser_step = FreezerDecoderStepReset;
            break;
        }
        if(subghz_protocol_freezer_is_short(instance->decoder.te_last) &&
           subghz_protocol_freezer_is_long(duration)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 1);
            instance->decoder.parser_step = FreezerDecoderStepSaveDuration;
        } else if(
            subghz_protocol_freezer_is_long(instance->decoder.te_last) &&
            subghz_protocol_freezer_is_short(duration)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 0);
            instance->decoder.parser_step = FreezerDecoderStepSaveDuration;
        } else {
            instance->last_data = 0;
            instance->decoder.parser_step = FreezerDecoderStepReset;
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
        }
        break;
    }
}

<<<<<<< HEAD
uint8_t subghz_protocol_decoder_freezer_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFreezer* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_freezer_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFreezer* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_freezer_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFreezer* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, FREEZER_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    freezer_fields(&instance->generic, instance->generic.data);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_freezer_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderFreezer* instance = context;
    furi_string_cat_printf(
        output,
        "%s 24bit\r\n"
        "Key:%06llX\r\n"
        "Sn:%05lX Btn:%01X\r\n"
        "Fixed code - no counter\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->generic.serial,
        instance->generic.btn);
}

/* --- Encoder --- */

static void freezer_emit(SubGhzProtocolEncoderFreezer* instance, bool level, uint32_t duration) {
    size_t size = instance->encoder.size_upload;
    LevelDuration* upload = instance->encoder.upload;
    if(size && level_duration_get_level(upload[size - 1]) == level) {
        upload[size - 1] =
            level_duration_make(level, duration + level_duration_get_duration(upload[size - 1]));
    } else if(size < FREEZER_UPLOAD_CAPACITY) {
        upload[size] = level_duration_make(level, duration);
        instance->encoder.size_upload++;
    } else {
        instance->overflow = true;
    }
}

static void freezer_build_frame(SubGhzProtocolEncoderFreezer* instance, uint64_t data) {
    freezer_emit(instance, true, FREEZER_TE_SHORT);
    for(int i = FREEZER_BITS - 1; i >= 0; i--) {
        bool one = ((data >> i) & 1u) != 0;
        freezer_emit(instance, false, one ? FREEZER_TE_SHORT : FREEZER_TE_LONG);
        freezer_emit(instance, true, one ? FREEZER_TE_LONG : FREEZER_TE_SHORT);
    }
    freezer_emit(instance, false, FREEZER_GAP_DURATION);
}

void* subghz_protocol_encoder_freezer_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFreezer* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_freezer;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_FREEZER_NAME;
    instance->encoder.upload = malloc(FREEZER_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    return instance;
}

void subghz_protocol_encoder_freezer_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFreezer* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_freezer_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFreezer* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_freezer_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFreezer* instance = context;
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
    subghz_protocol_encoder_freezer_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderFreezer* instance = context;
    subghz_protocol_encoder_freezer_stop(instance);

    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, FREEZER_BITS);
    if(status != SubGhzProtocolStatusOk) return status;

    instance->encoder.size_upload = 0;
    instance->overflow = false;
    for(uint32_t copy = 0; copy < FREEZER_TX_REPEAT; copy++) {
        freezer_build_frame(instance, instance->generic.data);
    }
    if(instance->overflow) return SubGhzProtocolStatusErrorEncoderGetUpload;

    uint32_t repeat = 1;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Repeat", &repeat, 1);
    instance->encoder.repeat = repeat;

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
=======
SubGhzProtocolStatus
    subghz_protocol_decoder_freezer_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderFreezer* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_freezer_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_freezer_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderFreezer* instance = context;
    subghz_protocol_freezer_check_remote_controller(&instance->generic);
    const SubGhzDecodeInput input = {
        .generic = &instance->generic,
    };
    subghz_decode_plugin_get_string("Freezer", &input, output);
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
}
