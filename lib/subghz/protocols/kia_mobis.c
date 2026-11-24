#include "kia_mobis.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#include <furi.h>
#include <string.h>
#include <stdlib.h>

/*
 * Kia Mobis: Hyundai Mobis's smart keys for Kia/Hyundai (2017-2019
 * boards), DECODE ONLY. KAT's own decoder (github.com/KaraZajac/KAT,
 * src/protocols/kia_mobis.rs), worked out from three real recordings (a
 * Kia Niro, K7 and Sportage QL, ten presses each); no other reference
 * reads this signal.
 *
 * Air format: 2-FSK Manchester, Te=194.8us. A burst: at least 6 one bits,
 * then a fixed 16-bit word (0x6AC2), then the 72-bit frame - found the
 * same way as gm_rfa.c (a rolling window + a run-length gate), except the
 * run here is of ONE bits, not zeros. No checksum exists, but unlike
 * kia_denso.c/kia_forte.c no repeat-confirmation is used either (KAT
 * reports every valid burst on its own) - the 16-bit word already makes a
 * chance match unlikely enough on its own.
 *
 * Frame (72 bits): a 28-bit press counter (goes up by one every press,
 * shared across all three fobs seen - not a per-fob identity), a 4-bit
 * button nibble (only 1 or 2 seen; which is which isn't known), then 40
 * bits that change with every press (no known cipher: KAT tried every
 * common CRC-8 polynomial, a sum and an XOR over the obvious spans and
 * none held). No fob ID in the clear.
 */

#define KIA_MOBIS_TE 195u
#define KIA_MOBIS_TOL 60u
#define KIA_MOBIS_WORD 0x6AC2u /* 16 bits */
#define KIA_MOBIS_WORD_BITS 16u
#define KIA_MOBIS_MIN_ONES 6u
#define KIA_MOBIS_FRAME_BYTES 9u /* 72 bits */
#define KIA_MOBIS_FRAME_BITS (KIA_MOBIS_FRAME_BYTES * 8u)
#define KIA_MOBIS_RESET_GAP 2500u

#define KIA_MOBIS_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < KIA_MOBIS_TOL)

typedef enum {
    KiaMobisStepSearching = 0,
    KiaMobisStepCollecting,
} KiaMobisDecoderStep;

struct SubGhzProtocolDecoderKiaMobis {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint32_t window; /* rolling last-16-bits window, for the word match */
    uint32_t one_run; /* consecutive one bits before `window` */
    uint8_t frame[KIA_MOBIS_FRAME_BYTES];
    uint8_t count;
};

const SubGhzProtocolDecoder subghz_protocol_kia_mobis_decoder = {
    .alloc = subghz_protocol_decoder_kia_mobis_alloc,
    .free = subghz_protocol_decoder_kia_mobis_free,
    .reset = subghz_protocol_decoder_kia_mobis_reset,
    .feed = subghz_protocol_decoder_kia_mobis_feed,
    .get_hash_data = subghz_protocol_decoder_kia_mobis_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_mobis_serialize,
    .deserialize = subghz_protocol_decoder_kia_mobis_deserialize,
    .get_string = subghz_protocol_decoder_kia_mobis_get_string,
};

/* Decode-only - the cipher is unknown. */
const SubGhzProtocolEncoder subghz_protocol_kia_mobis_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_kia_mobis = {
    .name = SUBGHZ_PROTOCOL_KIA_MOBIS_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_kia_mobis_decoder,
    .encoder = &subghz_protocol_kia_mobis_encoder,
};

void* subghz_protocol_decoder_kia_mobis_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKiaMobis* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_kia_mobis;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_KIA_MOBIS_NAME;
    return instance;
}

void subghz_protocol_decoder_kia_mobis_free(void* context) {
    furi_check(context);
    free(context);
}

static void kia_mobis_reset_search(SubGhzProtocolDecoderKiaMobis* instance) {
    instance->decoder.parser_step = KiaMobisStepSearching;
    instance->window = 0;
    instance->one_run = 0;
    instance->manchester_state = ManchesterStateMid1;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_kia_mobis_reset(void* context) {
    furi_check(context);
    kia_mobis_reset_search(context);
}

void subghz_protocol_decoder_kia_mobis_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderKiaMobis* instance = context;

    if(duration > KIA_MOBIS_RESET_GAP) {
        kia_mobis_reset_search(instance);
        return;
    }

    ManchesterEvent event;
    if(KIA_MOBIS_IN_TOL(duration, KIA_MOBIS_TE)) {
        event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    } else if(KIA_MOBIS_IN_TOL(duration, 2u * KIA_MOBIS_TE)) {
        event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    } else {
        kia_mobis_reset_search(instance);
        return;
    }

    bool bit;
    if(!manchester_advance(
           instance->manchester_state, event, &instance->manchester_state, &bit)) {
        return;
    }

    if(instance->decoder.parser_step == KiaMobisStepCollecting) {
        if(bit) instance->frame[instance->count / 8] |= (uint8_t)(0x80 >> (instance->count % 8));
        instance->count++;
        if(instance->count == KIA_MOBIS_FRAME_BITS) {
            const uint8_t* f = instance->frame;
            uint32_t counter_button =
                ((uint32_t)f[0] << 24) | ((uint32_t)f[1] << 16) | ((uint32_t)f[2] << 8) | f[3];
            instance->generic.cnt = (counter_button >> 4) & 0xFFFFu; /* low 16 bits, as KAT shows */
            instance->generic.btn = (uint8_t)(counter_button & 0xFu);
            uint64_t data = 0;
            for(int i = 1; i < 9; i++) data = (data << 8) | f[i];
            instance->generic.data = data;
            instance->generic.data_count_bit = 64;
            kia_mobis_reset_search(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
            return;
        }
        return;
    }

    /* Searching: does this bit complete a word match (the last 16 bits,
     * including this one) with at least MIN_ONES one-bits right before it? */
    uint32_t candidate =
        ((instance->window << 1) | (bit ? 1u : 0u)) & ((1u << KIA_MOBIS_WORD_BITS) - 1u);
    bool aging_one = ((instance->window >> (KIA_MOBIS_WORD_BITS - 1)) & 1u) != 0;
    if(candidate == KIA_MOBIS_WORD && instance->one_run >= KIA_MOBIS_MIN_ONES) {
        instance->decoder.parser_step = KiaMobisStepCollecting;
        memset(instance->frame, 0, sizeof(instance->frame));
        instance->count = 0;
        return;
    }
    instance->one_run = aging_one ? instance->one_run + 1 : 0;
    instance->window = candidate;
}

uint8_t subghz_protocol_decoder_kia_mobis_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaMobis* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < sizeof(instance->frame); i++) hash = (hash * 33u) ^ instance->frame[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_mobis_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderKiaMobis* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_mobis_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderKiaMobis* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
}

void subghz_protocol_decoder_kia_mobis_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderKiaMobis* instance = context;
    furi_string_cat_printf(
        output,
        "%s 72bit\r\n"
        "Cnt:%04lX Btn:%01X\r\n"
        "Code:%016llX\r\n"
        "Rolling - cipher unknown\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.cnt,
        instance->generic.btn,
        (unsigned long long)instance->generic.data);
}
