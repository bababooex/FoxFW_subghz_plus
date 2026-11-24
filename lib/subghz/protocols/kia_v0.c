#include "kia_v0.h"

#include "../blocks/const.h"
#include "../blocks/custom_btn_i.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <string.h>

// Kia V0 / Suzuki V0 / Honda V0 / Mitsubishi V0: one shared PWM 250/500us decoder, ported from
// this tree's own applications/fox/ProtoPirate/protocols/kia_v0.c (itself ProtoPirate's upstream
// kia_v0.c, after its "Refactor Kia V0 to include shared logic for Suzuki V0 and Honda V0" and
// "merge Mitsubishi protocol into Kia V0" commits - cross-checked against KAT's independent
// re-derivation, docs/kia_v0.md). Four frame types told apart by bit count/CRC/preamble:
//   1 Kia V0       - 61 bits, CRC-8 (poly 0x7F) over bits 8..55
//   2 Suzuki V0    - 64 bits, same CRC-8 but shifted key layout
//   3 Honda V0     - 61 wire bits that FAIL the Kia CRC, reinterpreted as a bit-reversed 64-bit
//                    key (structural check: nibble F/0, header low nibble 0xA)
//   4 Mitsubishi V0- a Kia frame sent twice, the second copy behind a 72..88-pair preamble
// This core decoder/encoder registers under the fixed name "KIA/HYU V0" (file round-trips always
// use that Protocol: name); the type-specific display name ("Suzuki V0"/"Honda V0"/"Mitsubishi V0")
// is shown in get_string() and saved in a "Type" field, not in the Protocol: line itself - a
// deliberate simplification vs. the C (which changes Protocol: per type) so re-loading a file this
// firmware saved always resolves through the one registered name.

#define TAG "SubGhzProtocolKiaV0"

static const SubGhzBlockConst subghz_protocol_kia_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 61,
};

#define KIA_V0_TYPE_KIA    1U
#define KIA_V0_TYPE_SUZUKI 2U
#define KIA_V0_TYPE_HONDA  3U
#define KIA_V0_TYPE_MITSU  4U

#define KIA_V0_BIT_COUNT_KIA    61U
#define KIA_V0_BIT_COUNT_SUZUKI 64U
#define KIA_V0_BIT_COUNT_HONDA  72U

#define KIA_V0_KIA_GAP      1000U
#define KIA_V0_KIA_GAP_BASE 700U
#define KIA_V0_KIA_GAP_SPAN 1000U

#define KIA_V0_SUZUKI_GAP      2000U
#define KIA_V0_SUZUKI_GAP_SPAN 500U

#define KIA_V0_TYPE1_SYNC                750U
#define KIA_V0_TYPE1_PREAMBLE_PAIRS      0x13FU
#define KIA_V0_TYPE2_PREAMBLE_PAIRS      0x140U
#define KIA_V0_TAIL_PREAMBLE_PAIRS       0x0FU
#define KIA_V0_MITSU_TAIL_PREAMBLE_PAIRS 0x50U
#define KIA_V0_MITSU_PREAMBLE_MIN        72U
#define KIA_V0_MITSU_PREAMBLE_MAX        88U
#define KIA_V0_HONDA_PREAMBLE_PAIRS      40U
#define KIA_V0_HONDA_SYNC_PAIRS          4U
#define KIA_V0_HONDA_WIRE_BITS           56U

#define KIA_V0_UPLOAD_CAPACITY                                                       \
    ((KIA_V0_TYPE2_PREAMBLE_PAIRS * 2U) + 2U + (KIA_V0_BIT_COUNT_SUZUKI * 2U) + 3U + \
     (KIA_V0_MITSU_TAIL_PREAMBLE_PAIRS * 2U) + (KIA_V0_BIT_COUNT_SUZUKI * 2U) + 4U)
#define KIA_V0_ENCODER_DEFAULT_REPEAT 3U

typedef enum {
    KiaV0DecoderStepReset = 0,
    KiaV0DecoderStepPreamble,
    KiaV0DecoderStepSaveDuration,
    KiaV0DecoderStepCheckDuration,
} KiaV0DecoderStep;

typedef struct {
    uint32_t serial;
    uint16_t counter;
    uint8_t button;
    uint8_t crc;
    uint8_t type;
    bool crc_valid;
} KiaV0Fields;

struct SubGhzProtocolDecoderKIA {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t preamble_pairs;
    uint16_t packet_bit_count;
    uint8_t type;
    uint64_t last_kia_data;
    bool have_last_kia;
};

struct SubGhzProtocolEncoderKIA {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint8_t type;
};

static const uint8_t kia_v0_honda_crc_table[16] = {
    0x4A,
    0x25,
    0x96,
    0x4B,
    0xA1,
    0xD4,
    0x6A,
    0x35,
    0x9E,
    0x4F,
    0xA3,
    0xD5,
    0xEE,
    0x77,
    0xBF,
    0xDB,
};

static const char* const kia_v0_honda_button_names[7] = {
    "Unlock",
    "Trunk",
    "Lock2",
    "Unlock2",
    "Trunk2",
    "Unlock3",
    "Trunk3",
};

#define kia_v0_crc8_poly(data, len) subghz_protocol_blocks_crc8((data), (len), 0x7F, 0x00)

static uint8_t kia_v0_reverse_bits8(uint8_t v) {
    v = (uint8_t)(((v & 0xF0U) >> 4U) | ((v & 0x0FU) << 4U));
    v = (uint8_t)(((v & 0xCCU) >> 2U) | ((v & 0x33U) << 2U));
    v = (uint8_t)(((v & 0xAAU) >> 1U) | ((v & 0x55U) << 1U));
    return v;
}

static void kia_v0_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t i = 0; i < 8U; i++) {
        bytes[i] = (uint8_t)(data >> ((7U - i) * 8U));
    }
}

static uint64_t kia_v0_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t i = 0; i < 8U; i++) {
        data = (data << 8U) | bytes[i];
    }
    return data;
}

static bool kia_v0_is_kia_gap(uint32_t duration) {
    return (duration >= KIA_V0_KIA_GAP_BASE) &&
           ((duration - KIA_V0_KIA_GAP_BASE) <= KIA_V0_KIA_GAP_SPAN);
}

static bool kia_v0_is_suzuki_gap_strict(uint32_t duration) {
    if(duration < KIA_V0_SUZUKI_GAP) return false;
    return (duration - KIA_V0_SUZUKI_GAP) <= KIA_V0_SUZUKI_GAP_SPAN;
}

static bool kia_v0_is_short(uint32_t duration) {
    return DURATION_DIFF(duration, subghz_protocol_kia_const.te_short) <
           (int32_t)subghz_protocol_kia_const.te_delta;
}

static bool kia_v0_is_long(uint32_t duration) {
    return DURATION_DIFF(duration, subghz_protocol_kia_const.te_long) <
           (int32_t)subghz_protocol_kia_const.te_delta;
}

static bool kia_v0_suzuki_verify_shifted_crc_words(uint32_t lo, uint32_t hi) {
    uint32_t r3 = ((lo >> 16) | (hi << 16)) & (uint32_t)~0xF0000000u;
    uint32_t r2 = (lo >> 12) & 0x0FU;
    uint32_t r1 = (hi >> 12) & 0xFFFFU;
    const uint8_t r4 = (uint8_t)((lo >> 4) & 0xFFU);

    r1 = ((r1 & 0xFFU) << 8) | ((r1 >> 8) & 0xFFU);

    uint8_t buf[6];
    buf[0] = (uint8_t)(r1 & 0xFFu);
    buf[1] = (uint8_t)((r1 >> 8) & 0xFFu);
    buf[2] = (uint8_t)((r3 >> 20) & 0xFFu);
    buf[3] = (uint8_t)((r3 >> 12) & 0xFFu);
    buf[4] = (uint8_t)((r3 >> 4) & 0xFFu);
    buf[5] = (uint8_t)(((r3 << 4) | (r2 & 0x0FU)) & 0xFFU);

    return (kia_v0_crc8_poly(buf, 6) == r4);
}

static bool kia_v0_suzuki_shifted_crc_valid(uint64_t shifted_key) {
    const uint32_t lo = (uint32_t)(shifted_key & 0xFFFFFFFFULL);
    const uint32_t hi = (uint32_t)((shifted_key >> 32) & 0xFFFFFFFFULL);
    return kia_v0_suzuki_verify_shifted_crc_words(lo, hi);
}

static uint8_t kia_v0_suzuki_crc8_from_fields(uint32_t serial, uint8_t button, uint32_t counter) {
    uint8_t buf[6];
    const uint16_t cnt_u16 = (uint16_t)(counter & 0xFFFFU);
    const uint16_t c_sw = (uint16_t)((cnt_u16 << 8) | (cnt_u16 >> 8));
    buf[0] = (uint8_t)(c_sw & 0xFFU);
    buf[1] = (uint8_t)((c_sw >> 8) & 0xFFU);
    buf[2] = (uint8_t)((serial >> 20) & 0xFFU);
    buf[3] = (uint8_t)((serial >> 12) & 0xFFU);
    buf[4] = (uint8_t)((serial >> 4) & 0xFFU);
    buf[5] = (uint8_t)((button & 0xFFU) | ((uint8_t)((uint32_t)serial << 4)));
    return kia_v0_crc8_poly(buf, 6);
}

static uint64_t kia_v0_suzuki_shifted_key_from_fields(
    uint32_t serial,
    uint8_t button,
    uint32_t counter,
    uint8_t crc_byte) {
    const uint32_t r8 = ((uint32_t)serial << 16) | ((uint32_t)crc_byte << 4) |
                        (((uint32_t)button & 0x0FU) << 12);
    const uint32_t r5 =
        (uint32_t)(((serial >> 16) & 0xFFFU) | (((uint32_t)(counter & 0xFFFFU)) << 12)) |
        0xF0000000U;
    return ((uint64_t)r5 << 32) | (uint64_t)r8;
}

static bool kia_v0_suzuki_resolve_shifted(uint64_t decode_data, uint64_t* out_shifted) {
    if(kia_v0_suzuki_shifted_crc_valid(decode_data)) {
        *out_shifted = decode_data;
        return true;
    }
    const uint64_t from_wire = decode_data << 1U;
    if(kia_v0_suzuki_shifted_crc_valid(from_wire)) {
        *out_shifted = from_wire;
        return true;
    }
    return false;
}

static uint8_t kia_v0_calculate_crc_poly(uint64_t data) {
    uint8_t crc_data[6];
    crc_data[0] = (data >> 48) & 0xFF;
    crc_data[1] = (data >> 40) & 0xFF;
    crc_data[2] = (data >> 32) & 0xFF;
    crc_data[3] = (data >> 24) & 0xFF;
    crc_data[4] = (data >> 16) & 0xFF;
    crc_data[5] = (data >> 8) & 0xFF;
    return kia_v0_crc8_poly(crc_data, 6);
}

static bool kia_v0_verify_crc_poly(uint64_t data) {
    uint8_t received_crc = data & 0xFF;
    return (kia_v0_calculate_crc_poly(data) == received_crc);
}

static uint64_t kia_v0_honda_transform(uint64_t data) {
    uint8_t bytes[8];
    kia_v0_u64_to_bytes_be(data, bytes);
    for(size_t index = 0; index < 8; index++) {
        bytes[index] = kia_v0_reverse_bits8(bytes[index]);
    }
    return kia_v0_bytes_to_u64_be(bytes);
}

static uint8_t kia_v0_honda_fold_counter(uint16_t counter) {
    uint8_t value = 0;
    for(size_t index = 0; index < COUNT_OF(kia_v0_honda_crc_table); index++) {
        if((counter >> index) & 1U) {
            value ^= kia_v0_honda_crc_table[index];
        }
    }
    return value;
}

static uint8_t kia_v0_honda_crc(uint8_t header, uint16_t counter) {
    uint8_t value = kia_v0_honda_fold_counter(counter);
    switch(header) {
    case 0xAA:
        value ^= 0xA5;
        break;
    case 0x2A:
        value ^= 0x21;
        break;
    case 0x6A:
        value ^= 0x15;
        break;
    case 0xFA:
        value ^= 0x73;
        break;
    default:
        value ^= 0xC6;
        break;
    }
    return value;
}

static uint8_t kia_v0_honda_header(uint8_t button) {
    const uint8_t button_3bit = button & 0x07U;
    const uint8_t base = (button_3bit == 0x07U) ? 0x1AU : 0x0AU;
    return (uint8_t)(((button_3bit << 5U) & 0xE0U) | base);
}

static uint64_t
    kia_v0_build_kia_raw(uint32_t serial, uint8_t button, uint16_t counter, uint8_t crc) {
    const uint32_t high = 0x0F000000UL | (((uint32_t)counter & 0xFFFFUL) << 8U) |
                          ((serial >> 20U) & 0xFFUL);
    const uint32_t low = (((uint32_t)serial & 0x000FFFFFUL) << 12U) |
                         (((uint32_t)button & 0x0FUL) << 8U) | crc;
    return ((uint64_t)high << 32U) | low;
}

static uint64_t kia_v0_build_honda_key(uint32_t serial, uint8_t button, uint16_t counter) {
    const uint8_t header = kia_v0_honda_header(button);
    const uint8_t crc = kia_v0_honda_crc(header, counter);
    const uint8_t bytes[8] = {
        0xF0,
        kia_v0_reverse_bits8((uint8_t)(counter >> 8U)),
        kia_v0_reverse_bits8((uint8_t)counter),
        (uint8_t)(serial >> 16U),
        (uint8_t)(serial >> 8U),
        (uint8_t)serial,
        header,
        crc,
    };
    return kia_v0_bytes_to_u64_be(bytes);
}

static bool kia_v0_honda_key_valid(uint64_t key) {
    return ((key >> 60U) == 0x0FULL) && (((key >> 56U) & 0x0FULL) == 0x00ULL) &&
           (((key >> 8U) & 0x0FULL) == 0x0AULL);
}

static uint8_t kia_v0_infer_type_from_bits(uint32_t bits) {
    if(bits == KIA_V0_BIT_COUNT_KIA) return KIA_V0_TYPE_KIA;
    if(bits == KIA_V0_BIT_COUNT_SUZUKI) return KIA_V0_TYPE_SUZUKI;
    if(bits == KIA_V0_BIT_COUNT_HONDA) return KIA_V0_TYPE_HONDA;
    return KIA_V0_TYPE_KIA;
}

static const char* kia_v0_protocol_name(uint8_t type) {
    switch(type) {
    case KIA_V0_TYPE_SUZUKI:
        return "Suzuki V0";
    case KIA_V0_TYPE_HONDA:
        return "Honda V0";
    case KIA_V0_TYPE_MITSU:
        return "Mitsubishi V0";
    default:
        return SUBGHZ_PROTOCOL_KIA_V0_NAME;
    }
}

static const char* kia_v0_button_name(uint8_t button, uint8_t type) {
    if(type == KIA_V0_TYPE_HONDA) {
        if((button >= 1U) && (button <= COUNT_OF(kia_v0_honda_button_names))) {
            return kia_v0_honda_button_names[button - 1U];
        }
        return "??";
    }
    if(type == KIA_V0_TYPE_SUZUKI) {
        switch(button) {
        case 0x03:
            return "Lock";
        case 0x04:
            return "Unlock";
        case 0x02:
            return "Trunk";
        case 0x01:
            return "Panic";
        default:
            return "??";
        }
    }
    switch(button) {
    case 0x01:
        return "Lock";
    case 0x02:
        return "Unlock";
    case 0x03:
        return "Trunk";
    case 0x04:
        return "Panic";
    default:
        return "??";
    }
}

static void kia_v0_parse_family_raw(uint64_t raw, uint8_t type, KiaV0Fields* fields) {
    fields->type = type;
    fields->serial = (type == KIA_V0_TYPE_SUZUKI) ? (uint32_t)((raw >> 16U) & 0x0FFFFFFFULL) :
                                                    (uint32_t)((raw >> 12U) & 0x0FFFFFFFULL);
    fields->button = (type == KIA_V0_TYPE_SUZUKI) ? (uint8_t)((raw >> 12U) & 0x0FU) :
                                                    (uint8_t)((raw >> 8U) & 0x0FU);
    fields->counter = (type == KIA_V0_TYPE_SUZUKI) ? (uint16_t)((raw >> 44U) & 0xFFFFU) :
                                                     (uint16_t)((raw >> 40U) & 0xFFFFU);
    fields->crc = (type == KIA_V0_TYPE_SUZUKI) ? (uint8_t)((raw >> 4U) & 0xFFU) :
                                                 (uint8_t)(raw & 0xFFU);
    fields->crc_valid = (type == KIA_V0_TYPE_SUZUKI) ? kia_v0_suzuki_shifted_crc_valid(raw) :
                                                       kia_v0_verify_crc_poly(raw);
}

static void kia_v0_parse_honda_key(uint64_t key, KiaV0Fields* fields) {
    uint8_t bytes[8];
    kia_v0_u64_to_bytes_be(key, bytes);
    fields->type = KIA_V0_TYPE_HONDA;
    fields->serial = ((uint32_t)bytes[3] << 16U) | ((uint32_t)bytes[4] << 8U) | (uint32_t)bytes[5];
    fields->counter = ((uint16_t)kia_v0_reverse_bits8(bytes[1]) << 8U) |
                      (uint16_t)kia_v0_reverse_bits8(bytes[2]);
    fields->button = bytes[6] >> 5U;
    fields->crc = bytes[7];
    fields->crc_valid = (kia_v0_honda_crc(bytes[6], fields->counter) == fields->crc);
}

static void kia_v0_parse_data(
    SubGhzBlockGeneric* generic,
    uint8_t type,
    KiaV0Fields* fields,
    uint16_t* packet_bit_count) {
    memset(fields, 0, sizeof(*fields));

    if(type == KIA_V0_TYPE_HONDA) {
        kia_v0_parse_honda_key(generic->data, fields);
        generic->data_count_bit = KIA_V0_BIT_COUNT_HONDA;
    } else {
        kia_v0_parse_family_raw(generic->data, type, fields);
        generic->data_count_bit = (type == KIA_V0_TYPE_SUZUKI) ? KIA_V0_BIT_COUNT_SUZUKI :
                                                                 KIA_V0_BIT_COUNT_KIA;
    }

    generic->serial = fields->serial;
    generic->cnt = fields->counter;
    generic->btn = fields->button;
    if(packet_bit_count) {
        *packet_bit_count = generic->data_count_bit;
    }
}

static uint8_t kia_v0_type_from_protocol_name(const char* name) {
    if(!name) return 0;
    if((strcmp(name, "Mitsu v0") == 0) || (strcmp(name, "Mitsu V0") == 0) ||
       (strcmp(name, "Mitsubishi v0") == 0) || (strcmp(name, "Mitsubishi V0") == 0)) {
        return KIA_V0_TYPE_MITSU;
    }
    if((strcmp(name, "Suzuki") == 0) || (strcmp(name, "Suzuki V0") == 0)) {
        return KIA_V0_TYPE_SUZUKI;
    }
    if((strcmp(name, "Honda v0") == 0) || (strcmp(name, "Honda V0") == 0)) {
        return KIA_V0_TYPE_HONDA;
    }
    return 0;
}

static uint64_t kia_v0_build_frame(uint8_t type, uint32_t serial, uint8_t button, uint16_t counter) {
    if(type == KIA_V0_TYPE_HONDA) {
        return kia_v0_build_honda_key(serial, button & 0x07U, counter);
    }
    if(type == KIA_V0_TYPE_SUZUKI) {
        const uint8_t crc = kia_v0_suzuki_crc8_from_fields(serial, button & 0x0FU, counter);
        return kia_v0_suzuki_shifted_key_from_fields(serial, button & 0x0FU, counter, crc);
    }
    const uint64_t partial = kia_v0_build_kia_raw(serial, button & 0x0FU, counter, 0);
    const uint8_t crc = kia_v0_calculate_crc_poly(partial);
    return kia_v0_build_kia_raw(serial, button & 0x0FU, counter, crc);
}

/* ---- Decoder ---- */

const SubGhzProtocolDecoder subghz_protocol_kia_decoder = {
    .alloc = subghz_protocol_decoder_kia_alloc,
    .free = subghz_protocol_decoder_kia_free,
    .feed = subghz_protocol_decoder_kia_feed,
    .reset = subghz_protocol_decoder_kia_reset,
    .get_hash_data = subghz_protocol_decoder_kia_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_serialize,
    .deserialize = subghz_protocol_decoder_kia_deserialize,
    .get_string = subghz_protocol_decoder_kia_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_kia_encoder = {
    .alloc = subghz_protocol_encoder_kia_alloc,
    .free = subghz_protocol_encoder_kia_free,
    .deserialize = subghz_protocol_encoder_kia_deserialize,
    .stop = subghz_protocol_encoder_kia_stop,
    .yield = subghz_protocol_encoder_kia_yield,
};

const SubGhzProtocol subghz_protocol_kia_v0 = {
    .name = SUBGHZ_PROTOCOL_KIA_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_kia_decoder,
    .encoder = &subghz_protocol_kia_encoder,
};

void* subghz_protocol_decoder_kia_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKIA* instance = calloc(1, sizeof(SubGhzProtocolDecoderKIA));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_kia_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_kia_free(void* context) {
    furi_check(context);
    free(context);
}

static void kia_v0_decoder_state_clear(SubGhzProtocolDecoderKIA* instance) {
    instance->decoder.parser_step = KiaV0DecoderStepReset;
    instance->decoder.te_last = 0;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_pairs = 0;
}

void subghz_protocol_decoder_kia_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKIA* instance = context;
    kia_v0_decoder_state_clear(instance);
    instance->type = 0;
    instance->have_last_kia = false;
    instance->last_kia_data = 0;
}

static void kia_v0_decoder_commit(
    SubGhzProtocolDecoderKIA* instance,
    uint64_t data,
    uint8_t type,
    uint16_t bit_count) {
    instance->type = type;
    instance->packet_bit_count = bit_count;
    instance->generic.data = data;
    instance->generic.data_count_bit = bit_count;

    KiaV0Fields scratch;
    kia_v0_parse_data(&instance->generic, type, &scratch, &instance->packet_bit_count);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }

    kia_v0_decoder_state_clear(instance);
}

static void kia_v0_decoder_finish_kia_or_honda_at_gap(SubGhzProtocolDecoderKIA* instance) {
    if(instance->decoder.decode_count_bit != KIA_V0_BIT_COUNT_KIA) {
        kia_v0_decoder_state_clear(instance);
        return;
    }

    const uint64_t data = instance->decoder.decode_data;
    if(kia_v0_verify_crc_poly(data)) {
        uint8_t type = KIA_V0_TYPE_KIA;
        if(instance->have_last_kia && (instance->last_kia_data == data) &&
           (instance->preamble_pairs >= KIA_V0_MITSU_PREAMBLE_MIN) &&
           (instance->preamble_pairs <= KIA_V0_MITSU_PREAMBLE_MAX)) {
            type = KIA_V0_TYPE_MITSU;
        }
        instance->last_kia_data = data;
        instance->have_last_kia = true;
        kia_v0_decoder_commit(instance, data, type, KIA_V0_BIT_COUNT_KIA);
        return;
    }

    const uint64_t raw = data & 0x0FFFFFFFFFFFFFFFULL;
    const uint64_t key = kia_v0_honda_transform(raw);
    if(kia_v0_honda_key_valid(key)) {
        kia_v0_decoder_commit(instance, key, KIA_V0_TYPE_HONDA, KIA_V0_BIT_COUNT_HONDA);
        return;
    }

    kia_v0_decoder_state_clear(instance);
}

static bool kia_v0_decoder_try_honda(SubGhzProtocolDecoderKIA* instance) {
    if(instance->decoder.decode_count_bit != KIA_V0_BIT_COUNT_KIA) return false;
    if(kia_v0_verify_crc_poly(instance->decoder.decode_data)) return false;

    const uint64_t raw = instance->decoder.decode_data & 0x0FFFFFFFFFFFFFFFULL;
    const uint64_t key = kia_v0_honda_transform(raw);
    if(!kia_v0_honda_key_valid(key)) return false;

    kia_v0_decoder_commit(instance, key, KIA_V0_TYPE_HONDA, KIA_V0_BIT_COUNT_HONDA);
    return true;
}

void subghz_protocol_decoder_kia_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderKIA* instance = context;

    switch(instance->decoder.parser_step) {
    case KiaV0DecoderStepReset:
        kia_v0_decoder_state_clear(instance);
        if(!level) break;
        if(!kia_v0_is_short(duration)) break;
        instance->decoder.parser_step = KiaV0DecoderStepPreamble;
        instance->decoder.te_last = duration;
        instance->preamble_pairs = 0;
        break;

    case KiaV0DecoderStepPreamble:
        if(level) {
            if(kia_v0_is_short(duration) || kia_v0_is_long(duration)) {
                instance->decoder.te_last = duration;
            } else {
                kia_v0_decoder_state_clear(instance);
            }
        } else if(kia_v0_is_short(duration) && kia_v0_is_short(instance->decoder.te_last)) {
            instance->preamble_pairs++;
        } else if(kia_v0_is_long(duration) && kia_v0_is_long(instance->decoder.te_last)) {
            if(instance->preamble_pairs > 14U) {
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                subghz_protocol_blocks_add_bit(&instance->decoder, 1U);
                subghz_protocol_blocks_add_bit(&instance->decoder, 1U);
                instance->decoder.parser_step = KiaV0DecoderStepSaveDuration;
            } else {
                kia_v0_decoder_state_clear(instance);
            }
        } else {
            kia_v0_decoder_state_clear(instance);
        }
        break;

    case KiaV0DecoderStepSaveDuration:
        if(!level) {
            kia_v0_decoder_state_clear(instance);
            break;
        }

        if(kia_v0_is_kia_gap(duration)) {
            kia_v0_decoder_finish_kia_or_honda_at_gap(instance);
            break;
        }

        if(kia_v0_is_suzuki_gap_strict(duration) &&
           kia_v0_is_suzuki_gap_strict(instance->decoder.te_last)) {
            if(instance->decoder.decode_count_bit == KIA_V0_BIT_COUNT_SUZUKI) {
                uint64_t shifted = 0;
                if(kia_v0_suzuki_resolve_shifted(instance->decoder.decode_data, &shifted)) {
                    kia_v0_decoder_commit(
                        instance, shifted, KIA_V0_TYPE_SUZUKI, KIA_V0_BIT_COUNT_SUZUKI);
                } else {
                    kia_v0_decoder_state_clear(instance);
                }
            } else {
                kia_v0_decoder_state_clear(instance);
            }
            break;
        }

        if(kia_v0_is_short(duration) || kia_v0_is_long(duration)) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = KiaV0DecoderStepCheckDuration;
        } else {
            kia_v0_decoder_state_clear(instance);
        }
        break;

    case KiaV0DecoderStepCheckDuration:
        if(level) {
            kia_v0_decoder_state_clear(instance);
            break;
        }

        if(kia_v0_is_short(instance->decoder.te_last) && kia_v0_is_short(duration)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 0U);
            if(!kia_v0_decoder_try_honda(instance)) {
                instance->decoder.parser_step = KiaV0DecoderStepSaveDuration;
            }
            break;
        }

        if(kia_v0_is_long(instance->decoder.te_last) && kia_v0_is_long(duration)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 1U);
            if(!kia_v0_decoder_try_honda(instance)) {
                instance->decoder.parser_step = KiaV0DecoderStepSaveDuration;
            }
            break;
        }

        if(kia_v0_is_suzuki_gap_strict(duration)) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = KiaV0DecoderStepSaveDuration;
            break;
        }

        if(!kia_v0_decoder_try_honda(instance)) {
            kia_v0_decoder_state_clear(instance);
        }
        break;

    default:
        kia_v0_decoder_state_clear(instance);
        break;
    }
}

uint8_t subghz_protocol_decoder_kia_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKIA* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->packet_bit_count > 64U ? 64U :
                                                                instance->packet_bit_count,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, 8U);
}

void subghz_protocol_decoder_kia_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderKIA* instance = context;
    KiaV0Fields fields;
    kia_v0_parse_data(&instance->generic, instance->type, &fields, &instance->packet_bit_count);

    subghz_custom_btn_set_original_if_unset(fields.button);
    subghz_custom_btn_set_max(instance->type == KIA_V0_TYPE_HONDA ? 3U : 4U);
    uint8_t custom_btn_id = subghz_custom_btn_get();
    uint8_t display_btn = (custom_btn_id == SUBGHZ_CUSTOM_BTN_OK) ? fields.button : custom_btn_id;
    if(instance->type == KIA_V0_TYPE_SUZUKI) {
        switch(custom_btn_id) {
        case SUBGHZ_CUSTOM_BTN_UP:
            display_btn = 0x03U;
            break;
        case SUBGHZ_CUSTOM_BTN_DOWN:
            display_btn = 0x04U;
            break;
        case SUBGHZ_CUSTOM_BTN_LEFT:
            display_btn = 0x02U;
            break;
        case SUBGHZ_CUSTOM_BTN_RIGHT:
            display_btn = 0x01U;
            break;
        default:
            break;
        }
    } else if(instance->type == KIA_V0_TYPE_HONDA) {
        switch(custom_btn_id) {
        case SUBGHZ_CUSTOM_BTN_UP:
            display_btn = 0x03U;
            break;
        case SUBGHZ_CUSTOM_BTN_DOWN:
            display_btn = 0x01U;
            break;
        case SUBGHZ_CUSTOM_BTN_LEFT:
            display_btn = 0x02U;
            break;
        default:
            break;
        }
    }

    const char* sn_fmt =
        (instance->type == KIA_V0_TYPE_HONDA) ?
            "%s %ubit\r\nKey:%016llX\r\nSn:%06lX Btn:%01X [%s]\r\nCnt:%04X CRC:%02X [%s]\r\n" :
            "%s %ubit\r\nKey:%016llX\r\nSn:%07lX Btn:%01X [%s]\r\nCnt:%04X CRC:%02X [%s]\r\n";
    furi_string_cat_printf(
        output,
        sn_fmt,
        kia_v0_protocol_name(instance->type),
        instance->packet_bit_count,
        (unsigned long long)instance->generic.data,
        (unsigned long)fields.serial,
        display_btn,
        kia_v0_button_name(display_btn, instance->type),
        fields.counter,
        fields.crc,
        fields.crc_valid ? "OK" : "ERR");
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderKIA* instance = context;
    KiaV0Fields scratch;
    kia_v0_parse_data(&instance->generic, instance->type, &scratch, &instance->packet_bit_count);

    /* Protocol: stays the fixed registered name (see this file's header comment) - only the
     * Type field and get_string()'s own text distinguish the four frame types. */
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_KIA_V0_NAME;
    SubGhzProtocolStatus status =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(status != SubGhzProtocolStatusOk) return status;

    uint32_t serial = instance->generic.serial;
    uint32_t btn = instance->generic.btn;
    uint32_t cnt = instance->generic.cnt;
    uint32_t type = instance->type ? instance->type : KIA_V0_TYPE_KIA;
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial, 1) ||
       !flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn, 1) ||
       !flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt, 1) ||
       !flipper_format_insert_or_update_uint32(flipper_format, "Type", &type, 1)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderKIA* instance = context;
    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(status != SubGhzProtocolStatusOk) return status;

    const uint32_t bits = instance->generic.data_count_bit;
    if(bits != KIA_V0_BIT_COUNT_KIA && bits != KIA_V0_BIT_COUNT_SUZUKI &&
       bits != KIA_V0_BIT_COUNT_HONDA) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    uint32_t type_u32 = kia_v0_infer_type_from_bits(bits);
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Type", &type_u32, 1) && type_u32 >= 1 &&
       type_u32 <= 4) {
        instance->type = (uint8_t)type_u32;
    } else {
        FuriString* proto = furi_string_alloc();
        uint8_t named = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_string(flipper_format, "Protocol", proto)) {
            named = kia_v0_type_from_protocol_name(furi_string_get_cstr(proto));
        }
        furi_string_free(proto);
        instance->type = named ? named : (uint8_t)kia_v0_infer_type_from_bits(bits);
    }

    KiaV0Fields scratch;
    kia_v0_parse_data(&instance->generic, instance->type, &scratch, &instance->packet_bit_count);

    uint32_t ser_u32 = 0, btn_u32 = 0, cnt_u32 = 0;
    flipper_format_rewind(flipper_format);
    const bool got_serial = flipper_format_read_uint32(flipper_format, "Serial", &ser_u32, 1);
    flipper_format_rewind(flipper_format);
    const bool got_btn = flipper_format_read_uint32(flipper_format, "Btn", &btn_u32, 1);
    flipper_format_rewind(flipper_format);
    const bool got_cnt = flipper_format_read_uint32(flipper_format, "Cnt", &cnt_u32, 1);

    if(got_serial || got_btn || got_cnt) {
        if(got_serial) instance->generic.serial = ser_u32;
        if(got_btn) instance->generic.btn = (uint8_t)btn_u32;
        if(got_cnt) instance->generic.cnt = (uint16_t)cnt_u32;

        instance->generic.data = kia_v0_build_frame(
            instance->type,
            instance->generic.serial,
            (uint8_t)instance->generic.btn,
            (uint16_t)instance->generic.cnt);
        instance->generic.data_count_bit = (instance->type == KIA_V0_TYPE_SUZUKI) ?
                                                KIA_V0_BIT_COUNT_SUZUKI :
                                            (instance->type == KIA_V0_TYPE_HONDA) ?
                                                KIA_V0_BIT_COUNT_HONDA :
                                                KIA_V0_BIT_COUNT_KIA;
        kia_v0_parse_data(
            &instance->generic, instance->type, &scratch, &instance->packet_bit_count);
    }

    return SubGhzProtocolStatusOk;
}

/* ---- Encoder ---- */

static size_t kia_v0_append_short_pairs(LevelDuration* upload, size_t index, size_t count) {
    for(size_t i = 0; i < count && index + 2U <= KIA_V0_UPLOAD_CAPACITY; i++) {
        upload[index++] = level_duration_make(true, (uint32_t)subghz_protocol_kia_const.te_short);
        upload[index++] = level_duration_make(false, (uint32_t)subghz_protocol_kia_const.te_short);
    }
    return index;
}

static size_t
    kia_v0_append_data_pairs(LevelDuration* upload, size_t index, uint64_t data, uint8_t bit_count) {
    for(int bit = bit_count - 1; bit >= 0 && index + 2U <= KIA_V0_UPLOAD_CAPACITY; bit--) {
        const uint32_t duration = ((data >> bit) & 1ULL) ?
                                      (uint32_t)subghz_protocol_kia_const.te_long :
                                      (uint32_t)subghz_protocol_kia_const.te_short;
        upload[index++] = level_duration_make(true, duration);
        upload[index++] = level_duration_make(false, duration);
    }
    return index;
}

static void kia_v0_build_kia_upload(SubGhzProtocolEncoderKIA* instance, uint64_t raw) {
    size_t index = 0;
    LevelDuration* up = instance->encoder.upload;

    up[index++] = level_duration_make(true, KIA_V0_TYPE1_SYNC);
    up[index++] = level_duration_make(false, KIA_V0_TYPE1_SYNC);
    index = kia_v0_append_short_pairs(up, index, KIA_V0_TYPE1_PREAMBLE_PAIRS);
    index = kia_v0_append_data_pairs(up, index, raw, KIA_V0_BIT_COUNT_KIA);
    up[index++] = level_duration_make(true, 1500);
    up[index++] = level_duration_make(false, 1500);
    index = kia_v0_append_short_pairs(up, index, KIA_V0_TAIL_PREAMBLE_PAIRS);
    index = kia_v0_append_data_pairs(up, index, raw, KIA_V0_BIT_COUNT_KIA);
    up[index++] = level_duration_make(true, KIA_V0_KIA_GAP);

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
}

static void kia_v0_build_mitsu_upload(SubGhzProtocolEncoderKIA* instance, uint64_t raw) {
    size_t index = 0;
    LevelDuration* up = instance->encoder.upload;

    up[index++] = level_duration_make(true, KIA_V0_TYPE1_SYNC);
    up[index++] = level_duration_make(false, KIA_V0_TYPE1_SYNC);
    index = kia_v0_append_short_pairs(up, index, KIA_V0_TYPE1_PREAMBLE_PAIRS);
    index = kia_v0_append_data_pairs(up, index, raw, KIA_V0_BIT_COUNT_KIA);
    up[index++] = level_duration_make(true, 1500);
    up[index++] = level_duration_make(false, 1500);
    index = kia_v0_append_short_pairs(up, index, KIA_V0_MITSU_TAIL_PREAMBLE_PAIRS);
    index = kia_v0_append_data_pairs(up, index, raw, KIA_V0_BIT_COUNT_KIA);
    up[index++] = level_duration_make(true, 1500);
    up[index++] = level_duration_make(false, 1500);

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
}

static void kia_v0_build_suzuki_upload(SubGhzProtocolEncoderKIA* instance, uint64_t shifted) {
    size_t index = 0;
    LevelDuration* up = instance->encoder.upload;

    index = kia_v0_append_short_pairs(up, index, KIA_V0_TYPE2_PREAMBLE_PAIRS);
    index = kia_v0_append_data_pairs(up, index, shifted, KIA_V0_BIT_COUNT_SUZUKI);
    up[index++] = level_duration_make(false, KIA_V0_SUZUKI_GAP);
    up[index++] = level_duration_make(true, KIA_V0_SUZUKI_GAP);
    up[index++] = level_duration_make(false, (uint32_t)subghz_protocol_kia_const.te_short);
    index = kia_v0_append_short_pairs(up, index, KIA_V0_TAIL_PREAMBLE_PAIRS);
    index = kia_v0_append_data_pairs(up, index, shifted, KIA_V0_BIT_COUNT_SUZUKI);

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
}

static void kia_v0_build_honda_upload(SubGhzProtocolEncoderKIA* instance, uint64_t raw) {
    size_t index = 0;
    LevelDuration* up = instance->encoder.upload;
    const uint64_t transformed = kia_v0_honda_transform(raw);

    index = kia_v0_append_short_pairs(up, index, KIA_V0_HONDA_PREAMBLE_PAIRS);
    for(size_t pair = 0; pair < KIA_V0_HONDA_SYNC_PAIRS; pair++) {
        up[index++] = level_duration_make(true, (uint32_t)subghz_protocol_kia_const.te_long);
        up[index++] = level_duration_make(false, (uint32_t)subghz_protocol_kia_const.te_long);
    }
    index = kia_v0_append_data_pairs(up, index, transformed, KIA_V0_HONDA_WIRE_BITS);
    up[index++] = level_duration_make(true, KIA_V0_KIA_GAP);

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
}

static void kia_v0_build_upload_for_type(SubGhzProtocolEncoderKIA* instance) {
    switch(instance->type) {
    case KIA_V0_TYPE_SUZUKI:
        kia_v0_build_suzuki_upload(instance, instance->generic.data);
        break;
    case KIA_V0_TYPE_HONDA:
        kia_v0_build_honda_upload(instance, instance->generic.data);
        break;
    case KIA_V0_TYPE_MITSU:
        kia_v0_build_mitsu_upload(instance, instance->generic.data);
        break;
    default:
        kia_v0_build_kia_upload(instance, instance->generic.data);
        break;
    }
}

void* subghz_protocol_encoder_kia_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderKIA* instance = calloc(1, sizeof(SubGhzProtocolEncoderKIA));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_kia_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = KIA_V0_ENCODER_DEFAULT_REPEAT;
    instance->encoder.upload = malloc(KIA_V0_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    return instance;
}

void subghz_protocol_encoder_kia_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKIA* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_kia_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKIA* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_kia_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKIA* instance = context;
    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }
    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    if(++instance->encoder.front == instance->encoder.size_upload) {
        instance->encoder.repeat--;
        instance->encoder.front = 0;
    }
    return ret;
}

void subghz_protocol_encoder_kia_set_button(void* context, uint8_t button) {
    furi_check(context);
    SubGhzProtocolEncoderKIA* instance = context;
    instance->generic.btn = (instance->type == KIA_V0_TYPE_HONDA) ? (button & 0x07U) :
                                                                    (button & 0x0FU);
}

void subghz_protocol_encoder_kia_set_counter(void* context, uint16_t counter) {
    furi_check(context);
    SubGhzProtocolEncoderKIA* instance = context;
    instance->generic.cnt = counter;
}

void subghz_protocol_encoder_kia_increment_counter(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKIA* instance = context;
    instance->generic.cnt = (instance->generic.cnt + 1U) & 0xFFFFU;
}

uint16_t subghz_protocol_encoder_kia_get_counter(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKIA* instance = context;
    return (uint16_t)(instance->generic.cnt & 0xFFFFU);
}

uint8_t subghz_protocol_encoder_kia_get_button(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKIA* instance = context;
    return (uint8_t)instance->generic.btn;
}

/* KAT's generic-button convention: for Kia/Mitsubishi the SUBGHZ_CUSTOM_BTN_UP/DOWN/LEFT/RIGHT
 * values (1/2/3/4) already equal that type's own Lock/Unlock/Trunk/Panic codes; Suzuki and Honda
 * use different native codes for the same four (three for Honda, which has no Panic) directions. */
static uint8_t kia_v0_map_button(uint8_t type, uint8_t custom_btn_id, uint8_t original_btn) {
    switch(type) {
    case KIA_V0_TYPE_SUZUKI:
        switch(custom_btn_id) {
        case SUBGHZ_CUSTOM_BTN_UP:
            return 0x03U;
        case SUBGHZ_CUSTOM_BTN_DOWN:
            return 0x04U;
        case SUBGHZ_CUSTOM_BTN_LEFT:
            return 0x02U;
        case SUBGHZ_CUSTOM_BTN_RIGHT:
            return 0x01U;
        default:
            return original_btn;
        }
    case KIA_V0_TYPE_HONDA:
        switch(custom_btn_id) {
        case SUBGHZ_CUSTOM_BTN_UP:
            return 0x03U;
        case SUBGHZ_CUSTOM_BTN_DOWN:
            return 0x01U;
        case SUBGHZ_CUSTOM_BTN_LEFT:
            return 0x02U;
        default:
            return original_btn;
        }
    default:
        switch(custom_btn_id) {
        case SUBGHZ_CUSTOM_BTN_UP:
            return 0x01U;
        case SUBGHZ_CUSTOM_BTN_DOWN:
            return 0x02U;
        case SUBGHZ_CUSTOM_BTN_LEFT:
            return 0x03U;
        case SUBGHZ_CUSTOM_BTN_RIGHT:
            return 0x04U;
        default:
            return original_btn;
        }
    }
}

SubGhzProtocolStatus
    subghz_protocol_encoder_kia_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderKIA* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    instance->encoder.repeat = KIA_V0_ENCODER_DEFAULT_REPEAT;

    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(status != SubGhzProtocolStatusOk) return status;

    const uint32_t bits = instance->generic.data_count_bit;
    if(bits != KIA_V0_BIT_COUNT_KIA && bits != KIA_V0_BIT_COUNT_SUZUKI &&
       bits != KIA_V0_BIT_COUNT_HONDA) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    uint32_t type_u32 = kia_v0_infer_type_from_bits(bits);
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Type", &type_u32, 1) && type_u32 >= 1 &&
       type_u32 <= 4) {
        instance->type = (uint8_t)type_u32;
    } else {
        FuriString* proto = furi_string_alloc();
        uint8_t named = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_string(flipper_format, "Protocol", proto)) {
            named = kia_v0_type_from_protocol_name(furi_string_get_cstr(proto));
        }
        furi_string_free(proto);
        instance->type = named ? named : (uint8_t)kia_v0_infer_type_from_bits(bits);
    }

    KiaV0Fields fields;
    kia_v0_parse_data(&instance->generic, instance->type, &fields, NULL);

    uint32_t ser_u32 = instance->generic.serial;
    uint32_t cnt_u32 = instance->generic.cnt;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Serial", &ser_u32, 1);
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Cnt", &cnt_u32, 1);

    /* Button remap: UP/DOWN/LEFT/RIGHT -> this type's native code; OK/no-override keeps the
     * captured button. This is a new port, so (unlike the enc_*.c bugs fixed earlier this
     * session) it is wired up to subghz_custom_btn_get() from the start. */
    subghz_custom_btn_set_original_if_unset(fields.button);
    subghz_custom_btn_set_max(instance->type == KIA_V0_TYPE_HONDA ? 3U : 4U);
    uint8_t button =
        kia_v0_map_button(instance->type, subghz_custom_btn_get(), fields.button);
    subghz_block_generic_global_button_override_get(&button);

    uint32_t cnt_mult = furi_hal_subghz_get_rolling_counter_mult();
    if(cnt_mult == 0U) cnt_mult = 1U;
    uint32_t override_cnt = 0;
    if(subghz_block_generic_global_counter_override_get(&override_cnt)) {
        cnt_u32 = override_cnt;
    } else {
        cnt_u32 += cnt_mult;
    }

    instance->generic.serial = ser_u32;
    instance->generic.btn = button;
    instance->generic.cnt = cnt_u32 & 0xFFFFU;
    instance->generic.data = kia_v0_build_frame(
        instance->type, instance->generic.serial, button, (uint16_t)instance->generic.cnt);
    instance->generic.data_count_bit = (instance->type == KIA_V0_TYPE_SUZUKI) ?
                                            KIA_V0_BIT_COUNT_SUZUKI :
                                        (instance->type == KIA_V0_TYPE_HONDA) ?
                                            KIA_V0_BIT_COUNT_HONDA :
                                            KIA_V0_BIT_COUNT_KIA;

    uint32_t repeat = KIA_V0_ENCODER_DEFAULT_REPEAT;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Repeat", &repeat, 1);
    instance->encoder.repeat = repeat ? repeat : KIA_V0_ENCODER_DEFAULT_REPEAT;

    kia_v0_build_upload_for_type(instance);
    if(instance->encoder.size_upload == 0) {
        return SubGhzProtocolStatusErrorEncoderGetUpload;
    }

    uint8_t key_bytes[8];
    kia_v0_u64_to_bytes_be(instance->generic.data, key_bytes);
    flipper_format_rewind(flipper_format);
    flipper_format_insert_or_update_hex(flipper_format, "Key", key_bytes, sizeof(key_bytes));
    flipper_format_rewind(flipper_format);
    flipper_format_insert_or_update_uint32(flipper_format, "Serial", &instance->generic.serial, 1);
    flipper_format_rewind(flipper_format);
    uint32_t btn_store = instance->generic.btn;
    flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_store, 1);
    flipper_format_rewind(flipper_format);
    flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &instance->generic.cnt, 1);
    flipper_format_rewind(flipper_format);
    uint32_t type_store = instance->type;
    flipper_format_insert_or_update_uint32(flipper_format, "Type", &type_store, 1);

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
