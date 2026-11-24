#include "faac_tm.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <stdlib.h>

/*
 * Faac TM - 12-bit DIP-switch fixed code (FAAC TM gate remotes). Ported
 * from RogueMaster's lib/subghz/protocols/faac_tm.c (added with
 * RogueMaster's merge of Unleashed's dev branch; no other reference has
 * it), cross-checked against the KAT project's independent re-derivation
 * (github.com/KaraZajac/KAT, src/protocols/faac_tm.rs).
 *
 * Air format (AM, 433.92/868 MHz): CAME's line coding, slower and with a
 * shorter header than CAME proper (hence its own decoder rather than
 * folding into came.c). Te is nominally 400us but MEASURED per frame (the
 * mean over the start bit and 12 data cells) and replayed back at that same
 * Te, since different remotes run at slightly different rates. A frame: a
 * header LOW of 35*Te, a start HIGH of one Te, then 12 bits MSB-first, each
 * a LOW then a HIGH: bit 1 = LOW Te + HIGH 2*Te, bit 0 = LOW 2*Te + HIGH Te
 * (the opposite polarity from proteco.c's cells - check this file's own
 * bit mapping, don't assume they match).
 *
 * No checksum, no counter, no button field - this is a 12-switch fixed
 * code, replayed exactly as captured. The measured Te is kept (as "TE" in
 * the key file) so a key saved at a remote's own slightly-off rate still
 * works when resent.
 */

#define FAAC_TM_TE_SHORT  400u
#define FAAC_TM_TE_LONG   800u
#define FAAC_TM_TE_DELTA  120u
#define FAAC_TM_BITS      12u
#define FAAC_TM_HEADER_TE 35u
/* A key file's TE field is used only strictly between these - out of range
 * (including the "Add Manually" sentinel 0) falls back to the nominal Te. */
#define FAAC_TM_TE_MIN 100u
#define FAAC_TM_TE_MAX 1000u
#define FAAC_TM_TX_REPEAT 3u
#define FAAC_TM_FRAME_PULSES (FAAC_TM_BITS * 2u + 2u)
#define FAAC_TM_UPLOAD_CAPACITY (FAAC_TM_FRAME_PULSES * FAAC_TM_TX_REPEAT)

#define FAAC_TM_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < FAAC_TM_TE_DELTA)

typedef enum {
    FaacTmStepReset = 0,
    FaacTmStepFoundStartBit,
    FaacTmStepSaveDuration,
    FaacTmStepCheckDuration,
} FaacTmDecoderStep;

struct SubGhzProtocolDecoderFaacTm {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint32_t te_sum;
    uint32_t measured_te;
};

struct SubGhzProtocolEncoderFaacTm {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint32_t measured_te;
    bool overflow;
};

/* The C's deserialize: the nominal rate unless a key's TE is strictly
 * between FAAC_TM_TE_MIN and FAAC_TM_TE_MAX. */
static uint32_t faac_tm_tx_te(uint32_t te) {
    return (te > FAAC_TM_TE_MIN && te < FAAC_TM_TE_MAX) ? te : FAAC_TM_TE_SHORT;
}

const SubGhzProtocolDecoder subghz_protocol_faac_tm_decoder = {
    .alloc = subghz_protocol_decoder_faac_tm_alloc,
    .free = subghz_protocol_decoder_faac_tm_free,
    .reset = subghz_protocol_decoder_faac_tm_reset,
    .feed = subghz_protocol_decoder_faac_tm_feed,
    .get_hash_data = subghz_protocol_decoder_faac_tm_get_hash_data,
    .serialize = subghz_protocol_decoder_faac_tm_serialize,
    .deserialize = subghz_protocol_decoder_faac_tm_deserialize,
    .get_string = subghz_protocol_decoder_faac_tm_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_faac_tm_encoder = {
    .alloc = subghz_protocol_encoder_faac_tm_alloc,
    .free = subghz_protocol_encoder_faac_tm_free,
    .deserialize = subghz_protocol_encoder_faac_tm_deserialize,
    .stop = subghz_protocol_encoder_faac_tm_stop,
    .yield = subghz_protocol_encoder_faac_tm_yield,
};

const SubGhzProtocol subghz_protocol_faac_tm = {
    .name = SUBGHZ_PROTOCOL_FAAC_TM_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_faac_tm_decoder,
    .encoder = &subghz_protocol_faac_tm_encoder,
};

void* subghz_protocol_decoder_faac_tm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFaacTm* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_faac_tm;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_FAAC_TM_NAME;
    return instance;
}

void subghz_protocol_decoder_faac_tm_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_faac_tm_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFaacTm* instance = context;
    instance->decoder.parser_step = FaacTmStepReset;
}

void subghz_protocol_decoder_faac_tm_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderFaacTm* instance = context;

    switch(instance->decoder.parser_step) {
    case FaacTmStepReset:
        if(!level &&
           DURATION_DIFF(duration, FAAC_TM_TE_SHORT * FAAC_TM_HEADER_TE) <
               FAAC_TM_TE_DELTA * FAAC_TM_HEADER_TE) {
            instance->decoder.parser_step = FaacTmStepFoundStartBit;
        }
        break;

    case FaacTmStepFoundStartBit:
        if(level && FAAC_TM_IN_TOL(duration, FAAC_TM_TE_SHORT)) {
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->te_sum = duration;
            instance->decoder.parser_step = FaacTmStepSaveDuration;
        } else if(level) {
            instance->decoder.parser_step = FaacTmStepReset;
        }
        break;

    case FaacTmStepSaveDuration:
        if(level) {
            instance->decoder.parser_step = FaacTmStepReset;
            break;
        }
        if(duration >= FAAC_TM_TE_SHORT * 4u) {
            /* A long LOW ends the frame. A complete, valid frame reports and
             * goes all the way back to Reset (so this same gap is NOT also
             * treated as the next frame's header) - a half-finished one
             * treats this gap as a fresh header instead. This exact
             * asymmetry (ported from the C/KAT) is why, of several
             * back-to-back copies while a button is held, every other one
             * decodes: the gap that closes a reported frame is "used up"
             * and the next copy's own start bit then fails Reset's check. */
            if(instance->decoder.decode_count_bit == FAAC_TM_BITS) {
                uint32_t te = instance->te_sum / (FAAC_TM_BITS * 3u + 1u);
                instance->generic.data = instance->decoder.decode_data & 0xFFFu;
                instance->generic.data_count_bit = FAAC_TM_BITS;
                instance->measured_te = te;
                instance->decoder.parser_step = FaacTmStepReset;
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            } else {
                instance->decoder.parser_step = FaacTmStepFoundStartBit;
            }
            break;
        }
        instance->decoder.te_last = duration;
        instance->te_sum += duration;
        instance->decoder.parser_step = FaacTmStepCheckDuration;
        break;

    case FaacTmStepCheckDuration:
        if(!level) {
            instance->decoder.parser_step = FaacTmStepReset;
            break;
        }
        /* bit 1: LOW short + HIGH long; bit 0: LOW long + HIGH short - the
         * opposite polarity from proteco.c. */
        if(FAAC_TM_IN_TOL(instance->decoder.te_last, FAAC_TM_TE_SHORT) &&
           FAAC_TM_IN_TOL(duration, FAAC_TM_TE_LONG)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 1);
        } else if(
            FAAC_TM_IN_TOL(instance->decoder.te_last, FAAC_TM_TE_LONG) &&
            FAAC_TM_IN_TOL(duration, FAAC_TM_TE_SHORT)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 0);
        } else {
            instance->decoder.parser_step = FaacTmStepReset;
            break;
        }
        instance->te_sum += duration;
        instance->decoder.parser_step = FaacTmStepSaveDuration;
        break;
    }
}

uint8_t subghz_protocol_decoder_faac_tm_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFaacTm* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_faac_tm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFaacTm* instance = context;
    SubGhzProtocolStatus status =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(status != SubGhzProtocolStatusOk) return status;
    uint32_t te = instance->measured_te;
    if(!flipper_format_insert_or_update_uint32(flipper_format, "TE", &te, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_faac_tm_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFaacTm* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, FAAC_TM_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    uint32_t te = 0;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "TE", &te, 1);
    instance->measured_te = te;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_faac_tm_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderFaacTm* instance = context;
    furi_string_cat_printf(
        output,
        "%s 12bit\r\n"
        "Key:%03llX Te:%luus\r\n"
        "DIP-switch fixed code\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->measured_te);
}

/* --- Encoder --- */

static void faac_tm_emit(SubGhzProtocolEncoderFaacTm* instance, bool level, uint32_t duration) {
    size_t size = instance->encoder.size_upload;
    LevelDuration* upload = instance->encoder.upload;
    if(size && level_duration_get_level(upload[size - 1]) == level) {
        upload[size - 1] =
            level_duration_make(level, duration + level_duration_get_duration(upload[size - 1]));
    } else if(size < FAAC_TM_UPLOAD_CAPACITY) {
        upload[size] = level_duration_make(level, duration);
        instance->encoder.size_upload++;
    } else {
        instance->overflow = true;
    }
}

static void faac_tm_build_frame(SubGhzProtocolEncoderFaacTm* instance, uint64_t data, uint32_t te) {
    faac_tm_emit(instance, false, te * FAAC_TM_HEADER_TE);
    faac_tm_emit(instance, true, te);
    for(int i = FAAC_TM_BITS - 1; i >= 0; i--) {
        bool one = ((data >> i) & 1u) != 0;
        faac_tm_emit(instance, false, one ? te : te * 2u);
        faac_tm_emit(instance, true, one ? te * 2u : te);
    }
}

void* subghz_protocol_encoder_faac_tm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFaacTm* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_faac_tm;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_FAAC_TM_NAME;
    instance->encoder.upload = malloc(FAAC_TM_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    return instance;
}

void subghz_protocol_encoder_faac_tm_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFaacTm* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_faac_tm_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFaacTm* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_faac_tm_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFaacTm* instance = context;
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
    subghz_protocol_encoder_faac_tm_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderFaacTm* instance = context;
    subghz_protocol_encoder_faac_tm_stop(instance);

    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, FAAC_TM_BITS);
    if(status != SubGhzProtocolStatusOk) return status;

    uint32_t stored_te = 0;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "TE", &stored_te, 1);
    uint32_t te = faac_tm_tx_te(stored_te);
    instance->measured_te = te;

    instance->encoder.size_upload = 0;
    instance->overflow = false;
    for(uint32_t copy = 0; copy < FAAC_TM_TX_REPEAT; copy++) {
        faac_tm_build_frame(instance, instance->generic.data, te);
    }
    if(instance->overflow) return SubGhzProtocolStatusErrorEncoderGetUpload;

    uint32_t repeat = 1;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Repeat", &repeat, 1);
    instance->encoder.repeat = repeat;

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
