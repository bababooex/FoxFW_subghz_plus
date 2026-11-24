#include "mercedes_v1.h"

#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"

#include "../blocks/custom_btn_i.h"

#include <furi.h>
#include <stdlib.h>
#include <string.h>

/*
 * Mercedes V1: a second Mercedes-Benz rolling-code frame - distinct from this tree's own
 * mercedes.c (Mercedes V0: 112 bits, 500/1000us). Adapted from a reference C port of this
 * decoder/encoder onto this tree's actual API - the original used a few things this codebase
 * doesn't have (a `SubGhzBlockGeneric` with an extra `extra`/`te` field, and a
 * `subghz_custom_btn_map_get()` lookup table limited to one override direction). Neither
 * field is load-bearing: the "repeat" flag bit is read from and written back to the frame
 * directly (no need to cache it in a separate field) and `te` is always the same fixed
 * constant. The button mapping below also uses the full UP/DOWN d-pad (this protocol only
 * has two states, Lock/Unlock) rather than the reference's single-direction toggle, matching
 * every other two-button protocol in this tree. The actual protocol logic (preamble hunt,
 * Manchester chip-pairing, field layout, frame-inversion handling) is unchanged.
 *
 * Air format (AM/FM, 433.92 MHz): the same Manchester "chip pair" scheme as bmw.c, one unit
 * at 50us, behind a preamble of at least 32 short pulses (no AGC step here, unlike BMW).
 *
 * Frame (122 bits, bit 0 first): bit0 always 1 (frame marker), bit1 = button type, bit2 =
 * state, bit3 = XNOR of bits 1/2 (a parity-style check), bit13 = a "repeat" flag carried
 * forward unchanged, bits[57:48] (split) = a 10-bit counter, bits[67:120] = a 54-bit "hop"
 * payload (cipher unknown, carried forward unchanged), bit121 always 1 (frame marker). A
 * frame may be captured bit-inverted; mercedes_v1_to_storage() detects and undoes that.
 */

#define MERCEDES_V1_TE_US            50
#define MERCEDES_V1_TE1_MIN_US       25
#define MERCEDES_V1_TE1_MAX_US       79
#define MERCEDES_V1_TE2_MIN_US       80
#define MERCEDES_V1_TE2_MAX_US       140
#define MERCEDES_V1_GAP_US           280
#define MERCEDES_V1_PREAMBLE_MIN     32
#define MERCEDES_V1_PAYLOAD_BITS     122
#define MERCEDES_V1_CNT_BITS         10
#define MERCEDES_V1_CNT_MASK         0x3FFU
#define MERCEDES_V1_HOP_MASK         0x3FFFFFFFFFFFFFULL
#define MERCEDES_V1_PREAMBLE_TX_BITS 561
#define MERCEDES_V1_REPEAT_DEFAULT   4
#define MERCEDES_V1_REPEAT_GAP_US    50000
#define MERCEDES_V1_UPLOAD_CAPACITY  3072
#define MERCEDES_V1_LO_INVERT_MASK   0xFFFFFFFFFFFFFFC0ULL
#define MERCEDES_V1_BTN_LOCK         0U
#define MERCEDES_V1_BTN_UNLOCK       1U
#define MERCEDES_V1_BTN_MAX          2

typedef enum {
    MercedesV1DecoderStepReset = 0,
    MercedesV1DecoderStepPreambleLow,
    MercedesV1DecoderStepPreambleHigh,
    MercedesV1DecoderStepData,
} MercedesV1DecoderStep;

struct SubGhzProtocolDecoderMercedesV1 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint64_t hi;
    uint64_t lo;
    uint16_t preamble_count;
    int8_t pair_chip;
    uint8_t nbits;
};

struct SubGhzProtocolEncoderMercedesV1 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

static void mercedes_v1_reset_soft(SubGhzProtocolDecoderMercedesV1* instance);
static bool mercedes_v1_get_bit(uint64_t hi, uint64_t lo, uint8_t i);
static uint16_t mercedes_v1_bits(uint64_t hi, uint64_t lo, uint8_t start, uint8_t len);
static void mercedes_v1_invert(uint64_t* hi, uint64_t* lo);
static bool mercedes_v1_fields_valid(uint64_t hi, uint64_t lo);
static bool mercedes_v1_to_storage(uint64_t* hi, uint64_t* lo);
static uint32_t mercedes_v1_get_counter(uint64_t hi, uint64_t lo);
static void mercedes_v1_set_counter(uint64_t* hi, uint64_t* lo, uint32_t cnt);

const SubGhzProtocolDecoder subghz_protocol_mercedes_v1_decoder = {
    .alloc = subghz_protocol_decoder_mercedes_v1_alloc,
    .free = subghz_protocol_decoder_mercedes_v1_free,
    .feed = subghz_protocol_decoder_mercedes_v1_feed,
    .reset = subghz_protocol_decoder_mercedes_v1_reset,
    .get_hash_data = subghz_protocol_decoder_mercedes_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_mercedes_v1_serialize,
    .deserialize = subghz_protocol_decoder_mercedes_v1_deserialize,
    .get_string = subghz_protocol_decoder_mercedes_v1_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_mercedes_v1_encoder = {
    .alloc = subghz_protocol_encoder_mercedes_v1_alloc,
    .free = subghz_protocol_encoder_mercedes_v1_free,
    .deserialize = subghz_protocol_encoder_mercedes_v1_deserialize,
    .stop = subghz_protocol_encoder_mercedes_v1_stop,
    .yield = subghz_protocol_encoder_mercedes_v1_yield,
};

const SubGhzProtocol subghz_protocol_mercedes_v1 = {
    .name = SUBGHZ_PROTOCOL_MERCEDES_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_mercedes_v1_decoder,
    .encoder = &subghz_protocol_mercedes_v1_encoder,
};

static bool mercedes_v1_is_1t(uint32_t duration) {
    return (duration >= MERCEDES_V1_TE1_MIN_US) && (duration <= MERCEDES_V1_TE1_MAX_US);
}

static bool mercedes_v1_is_2t(uint32_t duration) {
    return (duration >= MERCEDES_V1_TE2_MIN_US) && (duration <= MERCEDES_V1_TE2_MAX_US);
}

static uint8_t mercedes_v1_chips(uint32_t duration) {
    if(mercedes_v1_is_1t(duration)) return 1;
    if(mercedes_v1_is_2t(duration)) return 2;
    return 0;
}

static bool mercedes_v1_get_bit(uint64_t hi, uint64_t lo, uint8_t i) {
    if(i < 64) {
        return (hi >> (63U - i)) & 1U;
    }
    return (lo >> (63U - (i - 64U))) & 1U;
}

static void mercedes_v1_set_bit(uint64_t* hi, uint64_t* lo, uint8_t i, bool value) {
    if(i < 64) {
        uint64_t mask = 1ULL << (63U - i);
        if(value) {
            *hi |= mask;
        } else {
            *hi &= ~mask;
        }
    } else {
        uint64_t mask = 1ULL << (63U - (i - 64U));
        if(value) {
            *lo |= mask;
        } else {
            *lo &= ~mask;
        }
    }
}

static uint16_t mercedes_v1_bits(uint64_t hi, uint64_t lo, uint8_t start, uint8_t len) {
    uint16_t value = 0;
    for(uint8_t i = 0; i < len; i++) {
        value =
            (uint16_t)((value << 1) | (mercedes_v1_get_bit(hi, lo, (uint8_t)(start + i)) ? 1U : 0U));
    }
    return value;
}

static void mercedes_v1_invert(uint64_t* hi, uint64_t* lo) {
    *hi = ~(*hi);
    *lo ^= MERCEDES_V1_LO_INVERT_MASK;
}

static uint32_t mercedes_v1_get_counter(uint64_t hi, uint64_t lo) {
    uint32_t cnt = mercedes_v1_get_bit(hi, lo, 48) ? 0x200UL : 0UL;
    if(mercedes_v1_get_bit(hi, lo, 49)) cnt |= 0x100UL;
    for(uint8_t k = 0; k < 8; k++) {
        if(mercedes_v1_get_bit(hi, lo, (uint8_t)(57 + k))) {
            cnt |= (1UL << k);
        }
    }
    return cnt & MERCEDES_V1_CNT_MASK;
}

static void mercedes_v1_set_counter(uint64_t* hi, uint64_t* lo, uint32_t cnt) {
    cnt &= MERCEDES_V1_CNT_MASK;
    mercedes_v1_set_bit(hi, lo, 48, cnt & 0x200UL);
    mercedes_v1_set_bit(hi, lo, 49, cnt & 0x100UL);
    for(uint8_t k = 0; k < 8; k++) {
        mercedes_v1_set_bit(hi, lo, (uint8_t)(57 + k), (cnt >> k) & 1U);
    }
}

static bool mercedes_v1_fields_valid(uint64_t hi, uint64_t lo) {
    const bool btn = mercedes_v1_get_bit(hi, lo, 1);
    const bool counter_state = mercedes_v1_get_bit(hi, lo, 2);

    if(!mercedes_v1_get_bit(hi, lo, 0)) return false;
    if(mercedes_v1_get_bit(hi, lo, 3) != !(btn ^ counter_state)) return false;
    if(!mercedes_v1_get_bit(hi, lo, 121)) return false;
    return true;
}

static bool mercedes_v1_to_storage(uint64_t* hi, uint64_t* lo) {
    if(!mercedes_v1_get_bit(*hi, *lo, 0)) {
        mercedes_v1_invert(hi, lo);
    }
    return mercedes_v1_fields_valid(*hi, *lo);
}

/* serial/btn/cnt are re-derived from data/data_2 every time they're needed. The "repeat"
 * flag (bit 13) and the "hop" payload (bits [67:120]) aren't cached in separate fields -
 * mercedes_v1_rebuild_payload() reads them straight back out of data/data_2 before
 * overwriting the button/counter bits, and get_string() reads them the same way to print. */
static void mercedes_v1_parse_fields(SubGhzBlockGeneric* instance) {
    const uint64_t hi = instance->data;
    const uint64_t lo = instance->data_2;

    instance->serial = ((uint32_t)mercedes_v1_bits(hi, lo, 16, 16) << 16) |
                       mercedes_v1_bits(hi, lo, 32, 16);
    instance->btn = mercedes_v1_get_bit(hi, lo, 1) ? 1 : 0;
    instance->cnt = mercedes_v1_get_counter(hi, lo);
    instance->data_count_bit = MERCEDES_V1_PAYLOAD_BITS;
}

/* The inverse of mercedes_v1_parse_fields()'s button/counter extraction: keeps every other
 * bit (the "repeat" flag, the hop payload) exactly as data/data_2 already have it. */
static void mercedes_v1_rebuild_payload(SubGhzBlockGeneric* instance) {
    uint64_t hi = instance->data;
    uint64_t lo = instance->data_2;
    const bool type_bit = instance->btn & 1U;
    const bool state_bit = mercedes_v1_get_bit(hi, lo, 2);

    mercedes_v1_set_bit(&hi, &lo, 0, true);
    mercedes_v1_set_bit(&hi, &lo, 1, type_bit);
    mercedes_v1_set_bit(&hi, &lo, 3, !(type_bit ^ state_bit));
    mercedes_v1_set_counter(&hi, &lo, instance->cnt);
    mercedes_v1_set_bit(&hi, &lo, 121, true);

    instance->data = hi;
    instance->data_2 = lo;
    instance->data_count_bit = MERCEDES_V1_PAYLOAD_BITS;
}

static const char* mercedes_v1_btn_name(uint8_t btn) {
    return btn ? "Unlock" : "Lock";
}

static uint8_t mercedes_v1_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return MERCEDES_V1_BTN_UNLOCK;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return MERCEDES_V1_BTN_LOCK;
    default:
        return subghz_custom_btn_get_original() & 1U;
    }
}

static size_t
    mercedes_v1_emit_merge(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static size_t mercedes_v1_emit_bit(LevelDuration* up, size_t i, size_t cap, bool bit_value) {
    if(bit_value) {
        i = mercedes_v1_emit_merge(up, i, cap, true, MERCEDES_V1_TE_US);
        i = mercedes_v1_emit_merge(up, i, cap, false, MERCEDES_V1_TE_US);
    } else {
        i = mercedes_v1_emit_merge(up, i, cap, false, MERCEDES_V1_TE_US);
        i = mercedes_v1_emit_merge(up, i, cap, true, MERCEDES_V1_TE_US);
    }
    return i;
}

static bool mercedes_v1_air_bit(uint64_t hi, uint64_t lo, uint8_t i) {
    return !mercedes_v1_get_bit(hi, lo, i);
}

static size_t mercedes_v1_emit_burst(
    LevelDuration* up,
    size_t i,
    size_t cap,
    const SubGhzBlockGeneric* generic) {
    for(uint16_t n = 0; n < MERCEDES_V1_PREAMBLE_TX_BITS; n++) {
        i = mercedes_v1_emit_bit(up, i, cap, true);
    }
    for(uint8_t bit = 0; bit < MERCEDES_V1_PAYLOAD_BITS; bit++) {
        i = mercedes_v1_emit_bit(up, i, cap, mercedes_v1_air_bit(generic->data, generic->data_2, bit));
    }
    return i;
}

static bool mercedes_v1_get_upload(SubGhzProtocolEncoderMercedesV1* instance) {
    LevelDuration* up = instance->encoder.upload;
    const size_t cap = MERCEDES_V1_UPLOAD_CAPACITY;
    size_t index = 0;

    /* Counter override (Signal Settings' debug "Edit Counter"), else the normal
     * rolling_counter_mult advance - without this the same counter value would be resent
     * forever, matching this session's fixes elsewhere for the same bug class. */
    subghz_block_generic_advance_counter(&instance->generic.cnt, MERCEDES_V1_CNT_MASK);

    subghz_custom_btn_set_original_if_unset(instance->generic.btn);
    subghz_custom_btn_set_max(MERCEDES_V1_BTN_MAX);
    uint8_t btn = mercedes_v1_get_btn_code();
    subghz_block_generic_global_button_override_get(&btn);
    instance->generic.btn = btn & 1U;

    mercedes_v1_rebuild_payload(&instance->generic);

    index = mercedes_v1_emit_burst(up, index, cap, &instance->generic);
    index = mercedes_v1_emit_merge(up, index, cap, false, MERCEDES_V1_REPEAT_GAP_US);

    instance->encoder.size_upload = index;
    return index > 0 && index < cap;
}

void* subghz_protocol_encoder_mercedes_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderMercedesV1* instance = malloc(sizeof(SubGhzProtocolEncoderMercedesV1));
    memset(instance, 0, sizeof(SubGhzProtocolEncoderMercedesV1));
    instance->base.protocol = &subghz_protocol_mercedes_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = MERCEDES_V1_REPEAT_DEFAULT;
    instance->encoder.size_upload = MERCEDES_V1_UPLOAD_CAPACITY;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_mercedes_v1_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderMercedesV1* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_mercedes_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderMercedesV1* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, MERCEDES_V1_PAYLOAD_BITS);
    if(ret != SubGhzProtocolStatusOk) return ret;

    /* The generic deserialize above only reads "Key" (the high 64 bits) into data - data_2
     * (the low 64) is this protocol's own "Data" field, same convention as beninca_arc.c. */
    uint8_t lo_bytes[sizeof(uint64_t)] = {0};
    if(!flipper_format_rewind(flipper_format) ||
       !flipper_format_read_hex(flipper_format, "Data", lo_bytes, sizeof(lo_bytes))) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    instance->generic.data_2 = 0;
    for(size_t i = 0; i < sizeof(lo_bytes); i++) {
        instance->generic.data_2 = (instance->generic.data_2 << 8) | lo_bytes[i];
    }

    mercedes_v1_parse_fields(&instance->generic);
    subghz_custom_btn_set_original_if_unset(instance->generic.btn);
    subghz_custom_btn_set_max(MERCEDES_V1_BTN_MAX);

    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);
    instance->encoder.front = 0;

    if(!mercedes_v1_get_upload(instance)) return SubGhzProtocolStatusErrorEncoderGetUpload;

    /* Persist the advanced frame back - without this, a second press in the same
     * Transmitter session would keep re-deriving from the originally captured counter and
     * never actually advance (same bug class this session fixed in Mazda V0/Chrysler/
     * Fiat Multi - see each one's own fix for the full explanation). */
    uint8_t hi_bytes[sizeof(uint64_t)] = {0};
    for(size_t i = 0; i < sizeof(hi_bytes); i++) {
        hi_bytes[sizeof(hi_bytes) - i - 1] =
            (uint8_t)((instance->generic.data >> (i * 8)) & 0xFFU);
    }
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(flipper_format, "Key", hi_bytes, sizeof(hi_bytes))) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    for(size_t i = 0; i < sizeof(lo_bytes); i++) {
        lo_bytes[sizeof(lo_bytes) - i - 1] =
            (uint8_t)((instance->generic.data_2 >> (i * 8)) & 0xFFU);
    }
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(flipper_format, "Data", lo_bytes, sizeof(lo_bytes))) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_encoder_mercedes_v1_stop(void* context) {
    SubGhzProtocolEncoderMercedesV1* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_mercedes_v1_yield(void* context) {
    SubGhzProtocolEncoderMercedesV1* instance = context;
    if(instance->encoder.repeat == 0 || !instance->encoder.is_running ||
       instance->encoder.size_upload == 0) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }
    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }
    return ret;
}

void* subghz_protocol_decoder_mercedes_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderMercedesV1* instance = malloc(sizeof(SubGhzProtocolDecoderMercedesV1));
    memset(instance, 0, sizeof(SubGhzProtocolDecoderMercedesV1));
    instance->base.protocol = &subghz_protocol_mercedes_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    mercedes_v1_reset_soft(instance);
    return instance;
}

void subghz_protocol_decoder_mercedes_v1_free(void* context) {
    furi_check(context);
    free(context);
}

static void mercedes_v1_reset_soft(SubGhzProtocolDecoderMercedesV1* instance) {
    instance->decoder.parser_step = MercedesV1DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->hi = 0;
    instance->lo = 0;
    instance->preamble_count = 0;
    instance->pair_chip = -1;
    instance->nbits = 0;
}

void subghz_protocol_decoder_mercedes_v1_reset(void* context) {
    furi_check(context);
    mercedes_v1_reset_soft(context);
}

static void mercedes_v1_resync_rejected_frame(SubGhzProtocolDecoderMercedesV1* instance) {
    const bool last =
        mercedes_v1_get_bit(instance->hi, instance->lo, MERCEDES_V1_PAYLOAD_BITS - 1);
    uint16_t run = 0;
    for(uint8_t i = MERCEDES_V1_PAYLOAD_BITS; i > 0; i--) {
        if(mercedes_v1_get_bit(instance->hi, instance->lo, i - 1) != last) break;
        run++;
    }
    mercedes_v1_reset_soft(instance);
    if(run >= MERCEDES_V1_PREAMBLE_MIN) {
        instance->preamble_count = run;
        instance->decoder.parser_step = last ? MercedesV1DecoderStepPreambleHigh :
                                              MercedesV1DecoderStepPreambleLow;
    }
}

static void mercedes_v1_try_callback(SubGhzProtocolDecoderMercedesV1* instance) {
    if(instance->nbits != MERCEDES_V1_PAYLOAD_BITS) {
        mercedes_v1_reset_soft(instance);
        return;
    }

    uint64_t hi = instance->hi;
    uint64_t lo = instance->lo;
    if(!mercedes_v1_to_storage(&hi, &lo)) {
        hi = instance->hi;
        lo = instance->lo;
        mercedes_v1_invert(&hi, &lo);
        if(!mercedes_v1_to_storage(&hi, &lo)) {
            mercedes_v1_resync_rejected_frame(instance);
            return;
        }
    }

    instance->generic.data = hi;
    instance->generic.data_2 = lo;
    instance->generic.data_count_bit = MERCEDES_V1_PAYLOAD_BITS;
    mercedes_v1_parse_fields(&instance->generic);
    subghz_custom_btn_set_original_if_unset(instance->generic.btn);
    subghz_custom_btn_set_max(MERCEDES_V1_BTN_MAX);
    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
    mercedes_v1_reset_soft(instance);
}

static void mercedes_v1_add_logical_bit(SubGhzProtocolDecoderMercedesV1* instance, bool bit) {
    furi_assert(instance->nbits < MERCEDES_V1_PAYLOAD_BITS);
    mercedes_v1_set_bit(&instance->hi, &instance->lo, instance->nbits, bit);
    instance->nbits++;
    instance->decoder.decode_count_bit = instance->nbits;
    if(instance->nbits == MERCEDES_V1_PAYLOAD_BITS) {
        mercedes_v1_try_callback(instance);
    }
}

static void mercedes_v1_add_chip(SubGhzProtocolDecoderMercedesV1* instance, bool chip) {
    if(instance->pair_chip < 0) {
        instance->pair_chip = chip ? 1 : 0;
        return;
    }
    const uint8_t first = (uint8_t)instance->pair_chip;
    const uint8_t second = chip ? 1 : 0;
    instance->pair_chip = -1;
    if(first == 1 && second == 0) {
        mercedes_v1_add_logical_bit(instance, true);
    } else if(first == 0 && second == 1) {
        mercedes_v1_add_logical_bit(instance, false);
    } else {
        mercedes_v1_reset_soft(instance);
    }
}

static void mercedes_v1_enter_data(SubGhzProtocolDecoderMercedesV1* instance, bool level) {
    instance->decoder.parser_step = MercedesV1DecoderStepData;
    instance->hi = 0;
    instance->lo = 0;
    instance->nbits = 0;
    instance->decoder.decode_count_bit = 0;
    instance->pair_chip = level ? 1 : 0;
}

void subghz_protocol_decoder_mercedes_v1_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderMercedesV1* instance = context;

    if(duration >= MERCEDES_V1_GAP_US) {
        mercedes_v1_reset_soft(instance);
        return;
    }

    const uint8_t n = mercedes_v1_chips(duration);
    if(n == 0) {
        if(instance->decoder.parser_step != MercedesV1DecoderStepReset) {
            mercedes_v1_reset_soft(instance);
        }
        return;
    }

    switch(instance->decoder.parser_step) {
    case MercedesV1DecoderStepReset:
        if(level && n == 1) {
            instance->preamble_count = 0;
            instance->decoder.parser_step = MercedesV1DecoderStepPreambleLow;
        } else if(!level && n == 1) {
            instance->preamble_count = 0;
            instance->decoder.parser_step = MercedesV1DecoderStepPreambleHigh;
        }
        break;

    case MercedesV1DecoderStepPreambleLow:
        if(!level && n == 1) {
            instance->preamble_count++;
            instance->decoder.parser_step = MercedesV1DecoderStepPreambleHigh;
        } else if(!level && n == 2 && instance->preamble_count >= MERCEDES_V1_PREAMBLE_MIN) {
            mercedes_v1_enter_data(instance, false);
        } else {
            mercedes_v1_reset_soft(instance);
        }
        break;

    case MercedesV1DecoderStepPreambleHigh:
        if(level && n == 1) {
            instance->decoder.parser_step = MercedesV1DecoderStepPreambleLow;
        } else if(level && n == 2 && instance->preamble_count >= MERCEDES_V1_PREAMBLE_MIN) {
            mercedes_v1_enter_data(instance, true);
        } else {
            mercedes_v1_reset_soft(instance);
        }
        break;

    case MercedesV1DecoderStepData:
        for(uint8_t i = 0; i < n; i++) {
            if(instance->decoder.parser_step != MercedesV1DecoderStepData) {
                break;
            }
            mercedes_v1_add_chip(instance, level);
        }
        break;

    default:
        mercedes_v1_reset_soft(instance);
        break;
    }

    instance->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_mercedes_v1_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMercedesV1* instance = context;
    uint64_t mix = instance->generic.data ^ instance->generic.data_2;
    return (uint8_t)(mix ^ (mix >> 8) ^ (mix >> 16) ^ (mix >> 24) ^ (mix >> 32) ^ (mix >> 40));
}

SubGhzProtocolStatus subghz_protocol_decoder_mercedes_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderMercedesV1* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    /* data_2 (the low 64 bits) isn't handled by the generic serializer - same convention
     * as beninca_arc.c's own wide frame. */
    uint8_t lo_bytes[sizeof(uint64_t)] = {0};
    for(size_t i = 0; i < sizeof(lo_bytes); i++) {
        lo_bytes[sizeof(lo_bytes) - i - 1] =
            (uint8_t)((instance->generic.data_2 >> (i * 8)) & 0xFFU);
    }
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(flipper_format, "Data", lo_bytes, sizeof(lo_bytes))) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_mercedes_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderMercedesV1* instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, MERCEDES_V1_PAYLOAD_BITS);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint8_t lo_bytes[sizeof(uint64_t)] = {0};
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_hex(flipper_format, "Data", lo_bytes, sizeof(lo_bytes))) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    instance->generic.data_2 = 0;
    for(size_t i = 0; i < sizeof(lo_bytes); i++) {
        instance->generic.data_2 = (instance->generic.data_2 << 8) | lo_bytes[i];
    }

    mercedes_v1_parse_fields(&instance->generic);
    return ret;
}

void subghz_protocol_decoder_mercedes_v1_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderMercedesV1* instance = context;
    mercedes_v1_parse_fields(&instance->generic);

    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.btn_length_bit = 1;
    subghz_block_generic_global.cnt_length_bit = MERCEDES_V1_CNT_BITS;
    subghz_block_generic_global.current_btn = instance->generic.btn;
    subghz_block_generic_global.current_cnt = instance->generic.cnt;
    subghz_custom_btn_set_original_if_unset(instance->generic.btn);
    subghz_custom_btn_set_max(MERCEDES_V1_BTN_MAX);

    bool repeat_flag = !mercedes_v1_get_bit(instance->generic.data, instance->generic.data_2, 13);
    uint64_t hop = 0;
    for(uint8_t i = 67; i <= 120; i++) {
        hop = (hop << 1) |
              (mercedes_v1_get_bit(instance->generic.data, instance->generic.data_2, i) ? 1ULL :
                                                                                           0ULL);
    }

    furi_string_cat_printf(
        output,
        "%s 122bit\r\n"
        "Sn:%08lX\r\n"
        "Btn:%s Rpt:%u Cnt:%lu\r\n"
        "Hop:%014llX",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        mercedes_v1_btn_name(instance->generic.btn),
        repeat_flag ? 1U : 0U,
        (unsigned long)instance->generic.cnt,
        (unsigned long long)(hop & MERCEDES_V1_HOP_MASK));
}
