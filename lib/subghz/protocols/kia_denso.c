#include "kia_denso.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#include <furi.h>
#include <stdlib.h>

/*
 * Kia Denso: DENSO Korea's smart keys for Kia (EV6, K5, newer Forte, Niro;
 * 2019 on), DECODE ONLY. KAT's own decoder (github.com/KaraZajac/KAT,
 * src/protocols/kia_denso.rs), worked out from two real recordings (ten
 * presses each, FCC filings CQOFD00790/CQOFD01340/CQOFN01050); no other
 * reference reads this signal.
 *
 * Air format: 2-FSK Manchester, Te=174.8us. A press sends the same 112-bit
 * frame 5 times, back to back with NO gap between copies - so this decoder
 * finds the frame positionally (a fixed 20-bit lead pattern, the same on
 * every fob seen) and additionally requires the SAME 112 bits to repeat
 * immediately behind themselves before reporting (no checksum exists, so a
 * confirmed repeat is the only anti-noise gate - like freezer.c's "must
 * repeat once" rule, just applied to a found frame instead of a whole
 * capture). Only one tone-to-level polarity is decoded here (not both, as
 * KAT tries) - a deliberate scope limit, not an oversight.
 *
 * Frame (112 bits): a 20-bit lead (9 ones then 00100110101, the same on
 * both fobs KAT has seen), a 28-bit fob ID (constant per fob), 10 bits of
 * unknown purpose (the same in every press on both fobs - the button may
 * be hiding in here, but which bits were never confirmed, since the
 * buttons pressed in the recordings aren't known), then 53 bits that
 * change with every press (no known cipher) and a final bit that's always
 * 0 in every frame seen.
 */

#define KIA_DENSO_TE 175u
#define KIA_DENSO_TOL 55u
#define KIA_DENSO_LEAD 0xFF935u /* 20 bits */
#define KIA_DENSO_LEAD_BITS 20u
#define KIA_DENSO_EXTRA_BITS 48u /* lead(20) + fob(28) */
#define KIA_DENSO_CODE_BITS 64u
#define KIA_DENSO_FRAME_BITS (KIA_DENSO_EXTRA_BITS + KIA_DENSO_CODE_BITS)
#define KIA_DENSO_RESET_GAP 3000u

#define KIA_DENSO_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < KIA_DENSO_TOL)

typedef enum {
    KiaDensoStepSearching = 0,
    KiaDensoStepCollecting,
} KiaDensoDecoderStep;

struct SubGhzProtocolDecoderKiaDenso {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint32_t lead_window; /* rolling last-20-bits window, for the lead match */
    uint64_t extra48;
    uint64_t code64;
    uint8_t count;
    bool have_last;
    uint64_t last_extra48;
    uint64_t last_code64;
};

const SubGhzProtocolDecoder subghz_protocol_kia_denso_decoder = {
    .alloc = subghz_protocol_decoder_kia_denso_alloc,
    .free = subghz_protocol_decoder_kia_denso_free,
    .reset = subghz_protocol_decoder_kia_denso_reset,
    .feed = subghz_protocol_decoder_kia_denso_feed,
    .get_hash_data = subghz_protocol_decoder_kia_denso_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_denso_serialize,
    .deserialize = subghz_protocol_decoder_kia_denso_deserialize,
    .get_string = subghz_protocol_decoder_kia_denso_get_string,
};

/* Decode-only - the cipher is unknown. */
const SubGhzProtocolEncoder subghz_protocol_kia_denso_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_kia_denso = {
    .name = SUBGHZ_PROTOCOL_KIA_DENSO_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_kia_denso_decoder,
    .encoder = &subghz_protocol_kia_denso_encoder,
};

void* subghz_protocol_decoder_kia_denso_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKiaDenso* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_kia_denso;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_KIA_DENSO_NAME;
    return instance;
}

void subghz_protocol_decoder_kia_denso_free(void* context) {
    furi_check(context);
    free(context);
}

static void kia_denso_reset_search(SubGhzProtocolDecoderKiaDenso* instance, bool clear_last) {
    instance->decoder.parser_step = KiaDensoStepSearching;
    instance->lead_window = 0;
    instance->manchester_state = ManchesterStateMid1;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
    if(clear_last) instance->have_last = false;
}

void subghz_protocol_decoder_kia_denso_reset(void* context) {
    furi_check(context);
    kia_denso_reset_search(context, true);
}

void subghz_protocol_decoder_kia_denso_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderKiaDenso* instance = context;

    if(duration > KIA_DENSO_RESET_GAP) {
        kia_denso_reset_search(instance, true);
        return;
    }

    ManchesterEvent event;
    if(KIA_DENSO_IN_TOL(duration, KIA_DENSO_TE)) {
        event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    } else if(KIA_DENSO_IN_TOL(duration, 2u * KIA_DENSO_TE)) {
        event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    } else {
        kia_denso_reset_search(instance, true);
        return;
    }

    bool bit;
    if(!manchester_advance(
           instance->manchester_state, event, &instance->manchester_state, &bit)) {
        return;
    }

    if(instance->decoder.parser_step == KiaDensoStepCollecting) {
        if(instance->count < KIA_DENSO_EXTRA_BITS) {
            instance->extra48 = (instance->extra48 << 1) | (bit ? 1u : 0u);
        } else {
            instance->code64 = (instance->code64 << 1) | (bit ? 1u : 0u);
        }
        instance->count++;
        if(instance->count == KIA_DENSO_FRAME_BITS) {
            bool report = instance->have_last && instance->last_extra48 == instance->extra48 &&
                          instance->last_code64 == instance->code64;
            if(report) {
                instance->generic.serial = (uint32_t)(instance->extra48 & 0xFFFFFFFu);
                instance->generic.data = instance->code64;
                instance->generic.data_count_bit = KIA_DENSO_CODE_BITS;
            }
            instance->have_last = true;
            instance->last_extra48 = instance->extra48;
            instance->last_code64 = instance->code64;
            /* Keep have_last/last_* across this reset - only a reset_gap or
             * an outright decode error clears them (see kia_denso_reset_search). */
            kia_denso_reset_search(instance, false);
            if(report && instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        return;
    }

    /* Searching: does this bit complete a lead match (the last 20 bits,
     * including this one)? */
    uint32_t candidate =
        ((instance->lead_window << 1) | (bit ? 1u : 0u)) & ((1u << KIA_DENSO_LEAD_BITS) - 1u);
    if(candidate == KIA_DENSO_LEAD) {
        instance->decoder.parser_step = KiaDensoStepCollecting;
        instance->extra48 = KIA_DENSO_LEAD;
        instance->code64 = 0;
        instance->count = KIA_DENSO_LEAD_BITS;
        return;
    }
    instance->lead_window = candidate;
}

uint8_t subghz_protocol_decoder_kia_denso_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaDenso* instance = context;
    return subghz_protocol_blocks_get_hash_data(&instance->decoder, 14);
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_denso_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderKiaDenso* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_denso_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderKiaDenso* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, KIA_DENSO_CODE_BITS);
}

void subghz_protocol_decoder_kia_denso_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderKiaDenso* instance = context;
    furi_string_cat_printf(
        output,
        "%s 112bit\r\n"
        "Fob:%07lX\r\n"
        "Code:%016llX\r\n"
        "Rolling - cipher unknown\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        (unsigned long long)instance->generic.data);
}
