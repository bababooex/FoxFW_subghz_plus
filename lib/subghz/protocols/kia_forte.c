#include "kia_forte.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#include <furi.h>
#include <string.h>
#include <stdlib.h>

/*
 * Kia Forte: a 315 MHz Kia remote, DECODE ONLY. KAT's own decoder
 * (github.com/KaraZajac/KAT, src/protocols/kia_forte.rs), worked out from
 * one real recording (one fob, ten presses, two buttons); no other
 * reference reads this signal and no FCC filing has been matched to it.
 *
 * Air format: 2-FSK Manchester, Te=489.8us. A press is 4 separate bursts
 * (real gaps between them, ~90ms apart), so unlike kia_denso.c this
 * decoder uses an ordinary gap-based reset between frames. The frame is
 * found positionally: byte 0 is always exactly 0x01 (7 zero bits then a
 * 1), so a rolling 8-bit window matching 0x01 marks the frame start - no
 * separate preamble needed. No checksum exists, so - same idea as
 * freezer.c / kia_denso.c - a frame is only reported once the SAME 88 bits
 * were also seen in a previous burst less than 300ms earlier (a real
 * press's 4 copies are alike; unrelated noise isn't).
 *
 * Frame (88 bits / 11 bytes): byte 0 fixed (0x01), byte 1 the button
 * (only 0x01/0x02 seen, meaning unconfirmed beyond "two buttons"), bytes
 * 2..4 a fob ID (constant per fob), bytes 5..9 change with every press (no
 * known cipher), byte 10's low nibble fixed at 0xD (high nibble changes).
 */

#define KIA_FORTE_TE 490u
#define KIA_FORTE_TOL 150u
#define KIA_FORTE_HEAD 0x01u
#define KIA_FORTE_BYTES 11u
#define KIA_FORTE_BITS (KIA_FORTE_BYTES * 8u)
#define KIA_FORTE_RESET_GAP 2500u
#define KIA_FORTE_COPY_WITHIN_US 300000ull

#define KIA_FORTE_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < KIA_FORTE_TOL)

typedef enum {
    KiaForteStepSearching = 0,
    KiaForteStepCollecting,
} KiaForteDecoderStep;

struct SubGhzProtocolDecoderKiaForte {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint8_t window;
    uint8_t frame[KIA_FORTE_BYTES];
    uint8_t count;
    uint64_t clock_us;
    bool have_last;
    uint8_t last_frame[KIA_FORTE_BYTES];
    uint64_t last_clock_us;
};

const SubGhzProtocolDecoder subghz_protocol_kia_forte_decoder = {
    .alloc = subghz_protocol_decoder_kia_forte_alloc,
    .free = subghz_protocol_decoder_kia_forte_free,
    .reset = subghz_protocol_decoder_kia_forte_reset,
    .feed = subghz_protocol_decoder_kia_forte_feed,
    .get_hash_data = subghz_protocol_decoder_kia_forte_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_forte_serialize,
    .deserialize = subghz_protocol_decoder_kia_forte_deserialize,
    .get_string = subghz_protocol_decoder_kia_forte_get_string,
};

/* Decode-only - the cipher is unknown. */
const SubGhzProtocolEncoder subghz_protocol_kia_forte_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_kia_forte = {
    .name = SUBGHZ_PROTOCOL_KIA_FORTE_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_kia_forte_decoder,
    .encoder = &subghz_protocol_kia_forte_encoder,
};

void* subghz_protocol_decoder_kia_forte_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKiaForte* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_kia_forte;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_KIA_FORTE_NAME;
    return instance;
}

void subghz_protocol_decoder_kia_forte_free(void* context) {
    furi_check(context);
    free(context);
}

static void kia_forte_reset_search(SubGhzProtocolDecoderKiaForte* instance) {
    instance->decoder.parser_step = KiaForteStepSearching;
    instance->window = 0;
    instance->manchester_state = ManchesterStateMid1;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_kia_forte_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaForte* instance = context;
    kia_forte_reset_search(instance);
    instance->have_last = false;
    instance->clock_us = 0;
}

void subghz_protocol_decoder_kia_forte_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderKiaForte* instance = context;
    instance->clock_us += duration;

    if(duration > KIA_FORTE_RESET_GAP) {
        kia_forte_reset_search(instance);
        return;
    }

    ManchesterEvent event;
    if(KIA_FORTE_IN_TOL(duration, KIA_FORTE_TE)) {
        event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    } else if(KIA_FORTE_IN_TOL(duration, 2u * KIA_FORTE_TE)) {
        event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    } else {
        kia_forte_reset_search(instance);
        return;
    }

    bool bit;
    if(!manchester_advance(
           instance->manchester_state, event, &instance->manchester_state, &bit)) {
        return;
    }

    if(instance->decoder.parser_step == KiaForteStepCollecting) {
        if(bit) instance->frame[instance->count / 8] |= (uint8_t)(0x80 >> (instance->count % 8));
        instance->count++;
        if(instance->count == KIA_FORTE_BITS) {
            /* No checksum exists, so "the same 88 bits twice" isn't enough on its
             * own - a nearby unrelated signal that also repeats (another remote's
             * own retries) can alias into this protocol's wide, 8-bit-only sync
             * and still pass that check. In practice that produces a degenerate
             * all-0x00 or all-0xFF frame (bytes 1.. are a uniform run) - reject
             * it, same defense every other protocol ported this session has. */
            bool all_00 = true, all_ff = true;
            for(size_t i = 1; i < KIA_FORTE_BYTES; i++) {
                if(instance->frame[i] != 0x00u) all_00 = false;
                if(instance->frame[i] != 0xFFu) all_ff = false;
            }
            bool degenerate = all_00 || all_ff;
            bool report = !degenerate && instance->have_last &&
                          memcmp(instance->frame, instance->last_frame, KIA_FORTE_BYTES) == 0 &&
                          (instance->clock_us - instance->last_clock_us) < KIA_FORTE_COPY_WITHIN_US;
            if(report) {
                const uint8_t* f = instance->frame;
                instance->generic.btn = f[1];
                instance->generic.serial = ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 8) | f[4];
                uint64_t data = 0;
                for(int i = 3; i < 11; i++) data = (data << 8) | f[i];
                instance->generic.data = data;
                instance->generic.data_count_bit = 64;
            }
            memcpy(instance->last_frame, instance->frame, KIA_FORTE_BYTES);
            instance->last_clock_us = instance->clock_us;
            instance->have_last = true;
            kia_forte_reset_search(instance);
            if(report && instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        return;
    }

    instance->window = (uint8_t)((instance->window << 1) | (bit ? 1u : 0u));
    if(instance->window == KIA_FORTE_HEAD) {
        instance->decoder.parser_step = KiaForteStepCollecting;
        memset(instance->frame, 0, sizeof(instance->frame));
        instance->frame[0] = KIA_FORTE_HEAD;
        instance->count = 8;
    }
}

uint8_t subghz_protocol_decoder_kia_forte_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaForte* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < sizeof(instance->frame); i++) hash = (hash * 33u) ^ instance->frame[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_forte_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderKiaForte* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_forte_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderKiaForte* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
}

void subghz_protocol_decoder_kia_forte_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderKiaForte* instance = context;
    furi_string_cat_printf(
        output,
        "%s 88bit\r\n"
        "Fob:%06lX Btn:%02X\r\n"
        "Code:%016llX\r\n"
        "Rolling - cipher unknown\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        (unsigned long long)instance->generic.data);
}
