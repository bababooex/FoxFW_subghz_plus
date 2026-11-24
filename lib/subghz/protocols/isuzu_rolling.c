#include "isuzu_rolling.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#include <furi.h>
#include <string.h>
#include <stdlib.h>

/*
 * Isuzu Rolling: an Isuzu remote, DECODE ONLY. KAT's own decoder
 * (github.com/KaraZajac/KAT, src/protocols/isuzu_rolling.rs), worked out
 * from two real recordings (three presses each, lock and unlock); no other
 * reference reads this signal, and the model/year/FCC ID are not known.
 *
 * Air format: DIFFERENTIAL Manchester (like lexus_ux.c: decode plain
 * Manchester first via this firmware's manchester_decoder.c, then XOR each
 * raw bit with the one before it - see lexus_ux.c's header for why), half-
 * cell Te=405.6us. Unlike isuzu_static.c, there IS a real, clearly usable
 * gap between copies here: each frame takes ~85.6ms and copies start
 * ~105.3ms apart, leaving ~20ms of genuine silence - so this decoder uses
 * ordinary gap-based framing (reset, then collect positionally), not a
 * sliding window.
 *
 * Frame (105 bits): a 64-bit "code" that changes with every press (no
 * known cipher - KAT could not find a checksum of any kind over it), a
 * 28-bit fob ID (constant per remote), a 4-bit button nibble, 5 bits that
 * are always 0 (the only usable gate here, besides the differential
 * Manchester decode itself producing no violation), and 4 bits of
 * unexplained content that also change with every press.
 */

#define ISUZU_ROLLING_TE 406u
#define ISUZU_ROLLING_TOL 120u
#define ISUZU_ROLLING_RESET_GAP 3000u
#define ISUZU_ROLLING_FRAME_BITS 105u
#define ISUZU_ROLLING_FRAME_BYTES 14u /* 112 bits allocated, 105 used */

#define ISUZU_ROLLING_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < ISUZU_ROLLING_TOL)

typedef enum {
    IsuzuRollingStepIdle = 0,
    IsuzuRollingStepCollecting,
} IsuzuRollingDecoderStep;

struct SubGhzProtocolDecoderIsuzuRolling {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    bool have_prev_manchester;
    bool prev_manchester_bit;
    uint8_t frame[ISUZU_ROLLING_FRAME_BYTES];
    uint16_t count;
};

const SubGhzProtocolDecoder subghz_protocol_isuzu_rolling_decoder = {
    .alloc = subghz_protocol_decoder_isuzu_rolling_alloc,
    .free = subghz_protocol_decoder_isuzu_rolling_free,
    .reset = subghz_protocol_decoder_isuzu_rolling_reset,
    .feed = subghz_protocol_decoder_isuzu_rolling_feed,
    .get_hash_data = subghz_protocol_decoder_isuzu_rolling_get_hash_data,
    .serialize = subghz_protocol_decoder_isuzu_rolling_serialize,
    .deserialize = subghz_protocol_decoder_isuzu_rolling_deserialize,
    .get_string = subghz_protocol_decoder_isuzu_rolling_get_string,
};

/* Decode-only - the cipher is unknown. */
const SubGhzProtocolEncoder subghz_protocol_isuzu_rolling_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_isuzu_rolling = {
    .name = SUBGHZ_PROTOCOL_ISUZU_ROLLING_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_isuzu_rolling_decoder,
    .encoder = &subghz_protocol_isuzu_rolling_encoder,
};

static inline bool isuzu_rolling_get_bit(const uint8_t* frame, int pos) {
    return (frame[pos / 8] >> (7 - (pos % 8))) & 1u;
}

static uint32_t isuzu_rolling_bits(const uint8_t* frame, int start, int end) {
    uint32_t v = 0;
    for(int i = start; i < end; i++) v = (v << 1) | (isuzu_rolling_get_bit(frame, i) ? 1u : 0u);
    return v;
}

static bool isuzu_rolling_fields(SubGhzBlockGeneric* generic, const uint8_t* frame) {
    if(isuzu_rolling_bits(frame, 96, 101) != 0) return false; /* the 5 fixed zero bits */

    uint64_t code = 0;
    for(int i = 0; i < 8; i++) code = (code << 8) | frame[i];
    generic->data = code;
    generic->data_count_bit = 64;
    generic->serial = isuzu_rolling_bits(frame, 64, 92);
    generic->btn = (uint8_t)isuzu_rolling_bits(frame, 92, 96);
    generic->seed = isuzu_rolling_bits(frame, 101, 105); /* the 4 unexplained bits */
    return true;
}

void* subghz_protocol_decoder_isuzu_rolling_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderIsuzuRolling* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_isuzu_rolling;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_ISUZU_ROLLING_NAME;
    return instance;
}

void subghz_protocol_decoder_isuzu_rolling_free(void* context) {
    furi_check(context);
    free(context);
}

static void isuzu_rolling_reset_search(SubGhzProtocolDecoderIsuzuRolling* instance) {
    instance->decoder.parser_step = IsuzuRollingStepIdle;
    instance->have_prev_manchester = false;
    instance->manchester_state = ManchesterStateMid1;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_isuzu_rolling_reset(void* context) {
    furi_check(context);
    isuzu_rolling_reset_search(context);
}

void subghz_protocol_decoder_isuzu_rolling_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuRolling* instance = context;

    if(duration > ISUZU_ROLLING_RESET_GAP) {
        isuzu_rolling_reset_search(instance);
        return;
    }

    ManchesterEvent event;
    if(ISUZU_ROLLING_IN_TOL(duration, ISUZU_ROLLING_TE)) {
        event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    } else if(ISUZU_ROLLING_IN_TOL(duration, 2u * ISUZU_ROLLING_TE)) {
        event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    } else {
        isuzu_rolling_reset_search(instance);
        return;
    }

    bool mbit;
    if(!manchester_advance(instance->manchester_state, event, &instance->manchester_state, &mbit)) {
        return;
    }

    if(!instance->have_prev_manchester) {
        instance->prev_manchester_bit = mbit;
        instance->have_prev_manchester = true;
        instance->decoder.parser_step = IsuzuRollingStepCollecting;
        memset(instance->frame, 0, sizeof(instance->frame));
        instance->count = 0;
        return;
    }
    bool diff_bit = mbit != instance->prev_manchester_bit;
    instance->prev_manchester_bit = mbit;

    if(instance->decoder.parser_step != IsuzuRollingStepCollecting) return;

    if(diff_bit) instance->frame[instance->count / 8] |= (uint8_t)(0x80 >> (instance->count % 8));
    instance->count++;
    if(instance->count == ISUZU_ROLLING_FRAME_BITS) {
        bool valid = isuzu_rolling_fields(&instance->generic, instance->frame);
        isuzu_rolling_reset_search(instance);
        if(valid && instance->base.callback)
            instance->base.callback(&instance->base, instance->base.context);
    }
}

uint8_t subghz_protocol_decoder_isuzu_rolling_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuRolling* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < sizeof(instance->frame); i++) hash = (hash * 33u) ^ instance->frame[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_isuzu_rolling_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuRolling* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_isuzu_rolling_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuRolling* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
}

void subghz_protocol_decoder_isuzu_rolling_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderIsuzuRolling* instance = context;
    furi_string_cat_printf(
        output,
        "%s 105bit\r\n"
        "Fob:%07lX Btn:%01X\r\n"
        "Code:%016llX\r\n"
        "Rolling - cipher unknown\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        (unsigned long long)instance->generic.data);
}
