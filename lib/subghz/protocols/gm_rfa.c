#include "gm_rfa.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#include <furi.h>
#include <stdlib.h>

/*
 * GM RFA: the Omron "Remote Function Actuator" 315 MHz remotes used on GM
 * cars (Cadillac/GMC/Chevrolet/Buick, roughly 2004-2017), DECODE ONLY.
 * KAT's own decoder (github.com/KaraZajac/KAT, src/protocols/gm_rfa.rs),
 * worked out from two real recordings (a 2008-2012 Cadillac CTS and a GMC
 * Acadia); no other reference reads this signal.
 *
 * Air format: Manchester, Te=117.5us. A burst is a long run of zero bits
 * (at least 20), then the 84-bit frame (no gap between runs or bits within
 * one burst - the frame is found purely positionally, by its own 4-bit
 * head pattern after enough zeros, the same self-resyncing approach as
 * microchip_hcs362.c). A press repeats the whole burst roughly every
 * 100ms.
 *
 * Frame (84 bits): a 4-bit head (1101, the same on every fob seen), a
 * 16-bit fob ID (constant per remote), then a 64-bit rolling code of
 * unknown cipher - about half its bits differ from press to press, with no
 * known checksum and no button field in the clear.
 */

#define GM_RFA_TE 118u
#define GM_RFA_TE_DELTA 59u
#define GM_RFA_MIN_ZEROS 20u
#define GM_RFA_HEAD 0xDu /* 0b1101 */
#define GM_RFA_FOB_BITS 16u
#define GM_RFA_CODE_BITS 64u
#define GM_RFA_RESET_GAP 2000u

#define GM_RFA_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < GM_RFA_TE_DELTA)

typedef enum {
    GmRfaStepSearching = 0,
    GmRfaStepCollecting,
} GmRfaDecoderStep;

struct SubGhzProtocolDecoderGmRfa {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint8_t recent4; /* rolling last-4-bits window, for the head match */
    uint32_t zero_run; /* consecutive zero bits before `recent4` */
    uint32_t fob;
    uint64_t code;
    uint8_t count; /* 0..64, once collecting */
};

const SubGhzProtocolDecoder subghz_protocol_gm_rfa_decoder = {
    .alloc = subghz_protocol_decoder_gm_rfa_alloc,
    .free = subghz_protocol_decoder_gm_rfa_free,
    .reset = subghz_protocol_decoder_gm_rfa_reset,
    .feed = subghz_protocol_decoder_gm_rfa_feed,
    .get_hash_data = subghz_protocol_decoder_gm_rfa_get_hash_data,
    .serialize = subghz_protocol_decoder_gm_rfa_serialize,
    .deserialize = subghz_protocol_decoder_gm_rfa_deserialize,
    .get_string = subghz_protocol_decoder_gm_rfa_get_string,
};

/* Decode-only - the cipher is unknown, so nothing can be re-encoded. */
const SubGhzProtocolEncoder subghz_protocol_gm_rfa_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_gm_rfa = {
    .name = SUBGHZ_PROTOCOL_GM_RFA_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_gm_rfa_decoder,
    .encoder = &subghz_protocol_gm_rfa_encoder,
};

void* subghz_protocol_decoder_gm_rfa_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderGmRfa* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_gm_rfa;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_GM_RFA_NAME;
    return instance;
}

void subghz_protocol_decoder_gm_rfa_free(void* context) {
    furi_check(context);
    free(context);
}

static void gm_rfa_reset_search(SubGhzProtocolDecoderGmRfa* instance) {
    instance->decoder.parser_step = GmRfaStepSearching;
    instance->recent4 = 0;
    instance->zero_run = 0;
    instance->manchester_state = ManchesterStateMid1;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_gm_rfa_reset(void* context) {
    furi_check(context);
    gm_rfa_reset_search(context);
}

void subghz_protocol_decoder_gm_rfa_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderGmRfa* instance = context;

    if(duration > GM_RFA_RESET_GAP) {
        gm_rfa_reset_search(instance);
        return;
    }

    ManchesterEvent event;
    if(GM_RFA_IN_TOL(duration, GM_RFA_TE)) {
        event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    } else if(GM_RFA_IN_TOL(duration, 2u * GM_RFA_TE)) {
        event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    } else {
        gm_rfa_reset_search(instance);
        return;
    }

    bool bit;
    if(!manchester_advance(
           instance->manchester_state, event, &instance->manchester_state, &bit)) {
        return;
    }

    if(instance->decoder.parser_step == GmRfaStepCollecting) {
        if(instance->count < GM_RFA_FOB_BITS) {
            instance->fob = (instance->fob << 1) | (bit ? 1u : 0u);
        } else {
            instance->code = (instance->code << 1) | (bit ? 1u : 0u);
        }
        instance->count++;
        if(instance->count == GM_RFA_FOB_BITS + GM_RFA_CODE_BITS) {
            instance->generic.serial = instance->fob;
            instance->generic.data = instance->code;
            instance->generic.data_count_bit = GM_RFA_FOB_BITS + GM_RFA_CODE_BITS;
            gm_rfa_reset_search(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        return;
    }

    /* Searching: does this bit complete a HEAD match (the last 4 bits,
     * including this one) with at least MIN_ZEROS zeros right before it? */
    uint8_t candidate = (uint8_t)(((instance->recent4 << 1) | (bit ? 1u : 0u)) & 0xFu);
    bool aging_zero = ((instance->recent4 >> 3) & 1u) == 0;
    if(candidate == GM_RFA_HEAD && instance->zero_run >= GM_RFA_MIN_ZEROS) {
        instance->decoder.parser_step = GmRfaStepCollecting;
        instance->fob = 0;
        instance->code = 0;
        instance->count = 0;
        return;
    }
    /* The bit leaving the 4-bit window extends (or breaks) the zero run. */
    instance->zero_run = aging_zero ? instance->zero_run + 1 : 0;
    instance->recent4 = candidate;
}

uint8_t subghz_protocol_decoder_gm_rfa_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderGmRfa* instance = context;
    return subghz_protocol_blocks_get_hash_data(&instance->decoder, 10);
}

SubGhzProtocolStatus subghz_protocol_decoder_gm_rfa_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderGmRfa* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_gm_rfa_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderGmRfa* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, GM_RFA_CODE_BITS);
}

void subghz_protocol_decoder_gm_rfa_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderGmRfa* instance = context;
    furi_string_cat_printf(
        output,
        "%s 84bit\r\n"
        "Fob:%04lX\r\n"
        "Code:%016llX\r\n"
        "Rolling - cipher unknown\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        (unsigned long long)instance->generic.data);
}
