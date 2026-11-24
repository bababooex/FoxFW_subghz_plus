#include "suzuki_ts002.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include <lib/subghz/blocks/custom_btn_i.h>

#include <furi.h>
#include <stdlib.h>

/*
 * Suzuki TS002: Calsonic Kansei's two-button remote key for Suzuki (the
 * case reads "Calsonic Kansei Corp. TS002"). KAT's own decoder
 * (github.com/KaraZajac/KAT, src/protocols/suzuki_ts002.rs), worked out
 * from four real recordings (230 presses); no other reference reads this
 * signal. Not our suzuki.c (a different frame/timing).
 *
 * Decode + encode: the encoder is KAT's own "next press" rule (counter+1,
 * mod 512; Lock/Unlock, or the captured button again without its held
 * flag; the 13 fixed mask bits carried over unchanged) - KAT itself has
 * not tried this against a real car either, so treat a resynced remote
 * with the usual caution.
 *
 * Air format (AM, 433.91 MHz): pulse-width cells of 3 units (Te=256.8us):
 * bit 1 = HIGH 2 units + LOW 1 unit, bit 0 = HIGH 1 unit + LOW 2 units. A
 * press: 4 "0" cells, then the frame 4 times (a sync of 4 units carrier +
 * 4 units pause, 72 data bits, then 72 more cells that must ALL be "1" -
 * not stored, just a gate), then one more unit of carrier.
 *
 * Frame (72 bits): a fixed head byte (0x14 on the one remote recorded, not
 * itself checked here), a button byte (0x01 Lock, 0x02 Unlock, +0x20 once
 * held), a 24-bit ID, and a 32-bit "code". The code is NOT a cipher: it
 * holds the low 8 bits of the press counter (inverted, spread across four
 * bit-pairs - see C_POS) and, in the remaining 24 bits (R_POS), the ID
 * rotated left by that same counter byte and XORed with a mask built from
 * more counter bits, the button's low 2 bits, and 13 bits that stayed
 * constant across all 230 frames KAT read (FIXED_MASK / FIXED_SEEN) - the
 * gate is exactly reproducing that mask from the frame and checking it
 * against what the counter/button say it should be.
 */

#define SUZUKI_TS002_TE 257u
#define SUZUKI_TS002_TOL 110u
#define SUZUKI_TS002_SYNC_MIN 800u
#define SUZUKI_TS002_SYNC_MAX 1300u
#define SUZUKI_TS002_DATA_BITS 72u
#define SUZUKI_TS002_FILL_BITS 72u
#define SUZUKI_TS002_HELD 0x20u

static inline bool suzuki_ts002_is_short(uint32_t d) {
    uint32_t diff = (d > SUZUKI_TS002_TE) ? d - SUZUKI_TS002_TE : SUZUKI_TS002_TE - d;
    return diff <= SUZUKI_TS002_TOL;
}
static inline bool suzuki_ts002_is_long(uint32_t d) {
    uint32_t ref = 2u * SUZUKI_TS002_TE;
    uint32_t diff = (d > ref) ? d - ref : ref - d;
    return diff <= SUZUKI_TS002_TOL;
}
static inline bool suzuki_ts002_is_sync(uint32_t d) {
    return d >= SUZUKI_TS002_SYNC_MIN && d <= SUZUKI_TS002_SYNC_MAX;
}

/* Code bit that holds r0..r23 (r0 is the frame's first/MSB-most of the 24). */
static const uint8_t suzuki_ts002_r_pos[24] = {25, 24, 21, 20, 3,  2,  1,  0,  19, 18, 15, 14,
                                                13, 12, 11, 10, 7,  6,  5,  4,  29, 28, 27, 26};
/* Code bit that holds the inverse of counter bit 0..7. */
static const uint8_t suzuki_ts002_c_pos[8] = {30, 31, 16, 17, 8, 9, 22, 23};
/* Mask index (r number, 0 = bit 23) that holds the inverse of counter bit 0..8. */
static const uint8_t suzuki_ts002_c_mask[9] = {9, 8, 19, 18, 5, 4, 15, 14, 13};
/* Mask indices of the button byte's bit 1 and bit 0. */
static const uint8_t suzuki_ts002_b_mask[2] = {2, 3};
#define SUZUKI_TS002_FIXED_MASK 0xC338CFu
#define SUZUKI_TS002_FIXED_SEEN 0x0020C4u

typedef enum {
    SuzukiTs002StepIdle = 0,
    SuzukiTs002StepSyncLow,
    SuzukiTs002StepHigh,
    SuzukiTs002StepLow,
} SuzukiTs002DecoderStep;

struct SubGhzProtocolDecoderSuzukiTs002 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint32_t high; /* the duration of the cell's carrier half, while waiting for its pause */
    uint64_t hi64; /* the first 64 of the 72 data bits */
    uint8_t lo8; /* the last 8 of the 72 data bits */
    uint16_t count; /* 0..144 (72 data + 72 fill) */
};

static const char* const suzuki_ts002_names[4] = {"Lock", "Unlock", "Lock (held)", "Unlock (held)"};

const SubGhzProtocolDecoder subghz_protocol_suzuki_ts002_decoder = {
    .alloc = subghz_protocol_decoder_suzuki_ts002_alloc,
    .free = subghz_protocol_decoder_suzuki_ts002_free,
    .reset = subghz_protocol_decoder_suzuki_ts002_reset,
    .feed = subghz_protocol_decoder_suzuki_ts002_feed,
    .get_hash_data = subghz_protocol_decoder_suzuki_ts002_get_hash_data,
    .serialize = subghz_protocol_decoder_suzuki_ts002_serialize,
    .deserialize = subghz_protocol_decoder_suzuki_ts002_deserialize,
    .get_string = subghz_protocol_decoder_suzuki_ts002_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_suzuki_ts002_encoder = {
    .alloc = subghz_protocol_encoder_suzuki_ts002_alloc,
    .free = subghz_protocol_encoder_suzuki_ts002_free,
    .deserialize = subghz_protocol_encoder_suzuki_ts002_deserialize,
    .stop = subghz_protocol_encoder_suzuki_ts002_stop,
    .yield = subghz_protocol_encoder_suzuki_ts002_yield,
};

const SubGhzProtocol subghz_protocol_suzuki_ts002 = {
    .name = SUBGHZ_PROTOCOL_SUZUKI_TS002_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_suzuki_ts002_decoder,
    .encoder = &subghz_protocol_suzuki_ts002_encoder,
};

static uint32_t suzuki_ts002_rotl24(uint32_t value, uint32_t by) {
    by %= 24u;
    if(by == 0) return value & 0xFFFFFFu;
    return ((value << by) | (value >> (24u - by))) & 0xFFFFFFu;
}

static uint32_t suzuki_ts002_mask(uint16_t counter, uint8_t button) {
    uint32_t m = 0;
    for(int i = 0; i < 9; i++) {
        uint32_t bit = (~((uint32_t)counter >> i)) & 1u;
        m |= bit << (23 - suzuki_ts002_c_mask[i]);
    }
    m |= (uint32_t)((button >> 1) & 1u) << (23 - suzuki_ts002_b_mask[0]);
    m |= (uint32_t)(button & 1u) << (23 - suzuki_ts002_b_mask[1]);
    return m;
}

static bool suzuki_ts002_fields(SubGhzBlockGeneric* generic, uint64_t hi64, uint8_t lo8) {
    uint32_t id = (uint32_t)((hi64 >> 24) & 0xFFFFFFu);
    if(id == 0 || id == 0xFFFFFFu) return false;
    uint8_t button = (uint8_t)((hi64 >> 48) & 0xFFu);
    uint32_t code = (uint32_t)(((hi64 & 0xFFFFFFu) << 8) | lo8);

    uint16_t low = 0;
    for(int i = 0; i < 8; i++) {
        uint32_t bit = (~(code >> suzuki_ts002_c_pos[i])) & 1u;
        low |= (uint16_t)(bit << i);
    }
    uint32_t r = 0;
    for(int i = 0; i < 24; i++) {
        r = (r << 1) | ((code >> suzuki_ts002_r_pos[i]) & 1u);
    }
    uint32_t m = r ^ suzuki_ts002_rotl24(id, low);
    uint16_t counter = low;
    {
        uint32_t bit9 = (~(m >> (23 - suzuki_ts002_c_mask[8]))) & 1u;
        counter |= (uint16_t)(bit9 << 8);
    }
    uint32_t fixed_inv = (~(uint32_t)SUZUKI_TS002_FIXED_MASK) & 0xFFFFFFu;
    if((m & fixed_inv) != suzuki_ts002_mask(counter, button)) return false;

    uint32_t fixed = m & SUZUKI_TS002_FIXED_MASK;
    generic->data = hi64;
    generic->data_count_bit = 64;
    generic->seed = lo8;
    generic->serial = id;
    generic->btn = button;
    generic->cnt = counter;
    generic->data_2 = fixed; /* the 13 fixed mask bits, carried over unchanged by the encoder */
    generic->extra_check = (fixed == SUZUKI_TS002_FIXED_SEEN) ? 1 : 0;
    return true;
}

/* Bit `i` (0 = the frame's last/LSB-most bit, 71 = its first/MSB-most) of
 * the 72-bit frame split the same way the decoder accumulates it: the top
 * 64 bits in hi64, the bottom 8 in lo8. */
static inline bool suzuki_ts002_frame_bit(uint64_t hi64, uint8_t lo8, int i) {
    if(i >= 8) return ((hi64 >> (i - 8)) & 1u) != 0;
    return ((lo8 >> i) & 1u) != 0;
}

/* The inverse of suzuki_ts002_fields()'s de-obfuscation: rebuilds the
 * 72-bit frame (as hi64/lo8) from head/button/id/counter/fixed. Same
 * construction as KAT's own Fields::frame(). */
static void suzuki_ts002_build(
    uint8_t head,
    uint8_t button,
    uint32_t id,
    uint16_t counter,
    uint32_t fixed,
    uint64_t* hi64_out,
    uint8_t* lo8_out) {
    uint32_t r = suzuki_ts002_rotl24(id, counter & 0xFFu) ^ suzuki_ts002_mask(counter, button) ^
                 (fixed & SUZUKI_TS002_FIXED_MASK);
    uint32_t code = 0;
    for(int k = 0; k < 24; k++) {
        uint32_t bit = (r >> (23 - k)) & 1u;
        code |= bit << suzuki_ts002_r_pos[k];
    }
    for(int i = 0; i < 8; i++) {
        uint32_t bit = (~((uint32_t)counter >> i)) & 1u;
        code |= bit << suzuki_ts002_c_pos[i];
    }
    uint64_t hi64 = ((uint64_t)head << 56) | ((uint64_t)button << 48) |
                    ((uint64_t)(id & 0xFFFFFFu) << 24) | (uint64_t)((code >> 8) & 0xFFFFFFu);
    *hi64_out = hi64;
    *lo8_out = (uint8_t)(code & 0xFFu);
}

void* subghz_protocol_decoder_suzuki_ts002_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderSuzukiTs002* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_suzuki_ts002;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_SUZUKI_TS002_NAME;
    return instance;
}

void subghz_protocol_decoder_suzuki_ts002_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_suzuki_ts002_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSuzukiTs002* instance = context;
    instance->decoder.parser_step = SuzukiTs002StepIdle;
}

static void
    suzuki_ts002_restart(SubGhzProtocolDecoderSuzukiTs002* instance, bool level, uint32_t duration) {
    instance->decoder.parser_step =
        (level && suzuki_ts002_is_sync(duration)) ? SuzukiTs002StepSyncLow : SuzukiTs002StepIdle;
}

void subghz_protocol_decoder_suzuki_ts002_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderSuzukiTs002* instance = context;

    switch(instance->decoder.parser_step) {
    case SuzukiTs002StepIdle:
        suzuki_ts002_restart(instance, level, duration);
        break;

    case SuzukiTs002StepSyncLow:
        if(!level && suzuki_ts002_is_sync(duration)) {
            instance->hi64 = 0;
            instance->lo8 = 0;
            instance->count = 0;
            instance->decoder.parser_step = SuzukiTs002StepHigh;
        } else {
            suzuki_ts002_restart(instance, level, duration);
        }
        break;

    case SuzukiTs002StepHigh:
        if(level && (suzuki_ts002_is_short(duration) || suzuki_ts002_is_long(duration))) {
            instance->high = duration;
            instance->decoder.parser_step = SuzukiTs002StepLow;
        } else {
            suzuki_ts002_restart(instance, level, duration);
        }
        break;

    case SuzukiTs002StepLow: {
        bool bit;
        bool ok = true;
        if(!level && suzuki_ts002_is_long(instance->high) && suzuki_ts002_is_short(duration)) {
            bit = true;
        } else if(!level && suzuki_ts002_is_short(instance->high) && suzuki_ts002_is_long(duration)) {
            bit = false;
        } else {
            ok = false;
            bit = false;
        }
        if(!ok) {
            suzuki_ts002_restart(instance, level, duration);
            break;
        }

        if(instance->count < SUZUKI_TS002_DATA_BITS) {
            if(instance->count < 64) {
                instance->hi64 = (instance->hi64 << 1) | (bit ? 1u : 0u);
            } else {
                instance->lo8 = (uint8_t)((instance->lo8 << 1) | (bit ? 1u : 0u));
            }
        } else if(!bit) {
            /* A 0 among the all-1 fill bits: not a valid frame. */
            instance->decoder.parser_step = SuzukiTs002StepIdle;
            break;
        }
        instance->count++;
        instance->decoder.parser_step = SuzukiTs002StepHigh;
        if(instance->count == SUZUKI_TS002_DATA_BITS + SUZUKI_TS002_FILL_BITS) {
            instance->decoder.parser_step = SuzukiTs002StepIdle;
            if(suzuki_ts002_fields(&instance->generic, instance->hi64, instance->lo8)) {
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
        }
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_suzuki_ts002_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSuzukiTs002* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_suzuki_ts002_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderSuzukiTs002* instance = context;
    SubGhzProtocolStatus status =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(status != SubGhzProtocolStatusOk) return status;
    uint32_t lo8 = instance->generic.seed;
    if(!flipper_format_insert_or_update_uint32(flipper_format, "Lo8", &lo8, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_suzuki_ts002_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderSuzukiTs002* instance = context;
    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
    if(status != SubGhzProtocolStatusOk) return status;
    uint32_t lo8 = 0;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Lo8", &lo8, 1);
    if(!suzuki_ts002_fields(&instance->generic, instance->generic.data, (uint8_t)lo8))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_suzuki_ts002_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderSuzukiTs002* instance = context;
    subghz_custom_btn_set_original_if_unset(instance->generic.btn);
    subghz_custom_btn_set_max(2);
    subghz_block_generic_global_publish_btn(instance->generic.btn, 8);
    subghz_block_generic_global_publish_cnt(instance->generic.cnt, 9);
    uint8_t btn = instance->generic.btn;
    const char* name = (btn == 0x01)   ? suzuki_ts002_names[0] :
                       (btn == 0x02)   ? suzuki_ts002_names[1] :
                       (btn == 0x21)   ? suzuki_ts002_names[2] :
                       (btn == 0x22)   ? suzuki_ts002_names[3] :
                                         "?";
    furi_string_cat_printf(
        output,
        "%s 72bit\r\n"
        "Sn:%06lX Btn:%02X %s\r\n"
        "Cnt:%03lX\r\n"
        "Fixed bits: %s\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        btn,
        name,
        (unsigned long)instance->generic.cnt,
        instance->generic.extra_check ? "match recorded remote" : "DIFFER (unverified)");
}

/* --- Encoder --- */

#define SUZUKI_TS002_COPIES 4u
#define SUZUKI_TS002_LEAD_ZEROS 4u
#define SUZUKI_TS002_FINAL_GAP 10000u
#define SUZUKI_TS002_UPLOAD_CAPACITY                                                \
    (2u * SUZUKI_TS002_LEAD_ZEROS +                                                 \
     SUZUKI_TS002_COPIES * (2u + 2u * SUZUKI_TS002_DATA_BITS + 2u * SUZUKI_TS002_FILL_BITS) + \
     4u)

struct SubGhzProtocolEncoderSuzukiTs002 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    bool overflow;
};

static void
    suzuki_ts002_emit(SubGhzProtocolEncoderSuzukiTs002* instance, bool level, uint32_t duration) {
    size_t size = instance->encoder.size_upload;
    LevelDuration* upload = instance->encoder.upload;
    if(size && level_duration_get_level(upload[size - 1]) == level) {
        upload[size - 1] =
            level_duration_make(level, duration + level_duration_get_duration(upload[size - 1]));
    } else if(size < SUZUKI_TS002_UPLOAD_CAPACITY) {
        upload[size] = level_duration_make(level, duration);
        instance->encoder.size_upload++;
    } else {
        instance->overflow = true;
    }
}

/* A press: 4 "0" cells, the frame SUZUKI_TS002_COPIES times back-to-back
 * (each its own sync + 72 data bits + 72 "1" fill bits, no gap between
 * copies), then one more unit of carrier and the final gap - same as
 * KAT's own upload(). */
static void
    suzuki_ts002_transmit(SubGhzProtocolEncoderSuzukiTs002* instance, uint64_t hi64, uint8_t lo8) {
    for(uint32_t i = 0; i < SUZUKI_TS002_LEAD_ZEROS; i++) {
        suzuki_ts002_emit(instance, true, SUZUKI_TS002_TE);
        suzuki_ts002_emit(instance, false, 2u * SUZUKI_TS002_TE);
    }
    for(uint32_t copy = 0; copy < SUZUKI_TS002_COPIES; copy++) {
        suzuki_ts002_emit(instance, true, 4u * SUZUKI_TS002_TE);
        suzuki_ts002_emit(instance, false, 4u * SUZUKI_TS002_TE);
        for(int i = (int)SUZUKI_TS002_DATA_BITS - 1; i >= 0; i--) {
            bool one = suzuki_ts002_frame_bit(hi64, lo8, i);
            suzuki_ts002_emit(instance, true, one ? 2u * SUZUKI_TS002_TE : SUZUKI_TS002_TE);
            suzuki_ts002_emit(instance, false, one ? SUZUKI_TS002_TE : 2u * SUZUKI_TS002_TE);
        }
        for(uint32_t i = 0; i < SUZUKI_TS002_FILL_BITS; i++) {
            suzuki_ts002_emit(instance, true, 2u * SUZUKI_TS002_TE);
            suzuki_ts002_emit(instance, false, SUZUKI_TS002_TE);
        }
    }
    suzuki_ts002_emit(instance, true, SUZUKI_TS002_TE);
    suzuki_ts002_emit(instance, false, SUZUKI_TS002_FINAL_GAP);
}

void* subghz_protocol_encoder_suzuki_ts002_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderSuzukiTs002* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_suzuki_ts002;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_SUZUKI_TS002_NAME;
    instance->encoder.upload = malloc(SUZUKI_TS002_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    return instance;
}

void subghz_protocol_encoder_suzuki_ts002_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderSuzukiTs002* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_suzuki_ts002_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderSuzukiTs002* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_suzuki_ts002_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderSuzukiTs002* instance = context;
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

SubGhzProtocolStatus subghz_protocol_encoder_suzuki_ts002_deserialize(
    void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderSuzukiTs002* instance = context;
    subghz_protocol_encoder_suzuki_ts002_stop(instance);

    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
    if(status != SubGhzProtocolStatusOk) return status;
    uint32_t lo8_u32 = 0;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Lo8", &lo8_u32, 1);
    uint8_t lo8 = (uint8_t)lo8_u32;
    if(!suzuki_ts002_fields(&instance->generic, instance->generic.data, lo8))
        return SubGhzProtocolStatusErrorParserOthers;

    uint8_t captured_button = instance->generic.btn;
    subghz_custom_btn_set_original_if_unset(captured_button);
    uint8_t button;
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        button = 0x01; /* Lock */
        break;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        button = 0x02; /* Unlock */
        break;
    default:
        button = subghz_custom_btn_get_original() & (uint8_t)~SUZUKI_TS002_HELD;
        break;
    }
    if(button != 0x01 && button != 0x02) button = captured_button & (uint8_t)~SUZUKI_TS002_HELD;

    uint8_t head = (uint8_t)((instance->generic.data >> 56) & 0xFFu);
    uint32_t fixed = (uint32_t)instance->generic.data_2;

    /* "Next press": the counter advances (9 bits, wraps); the 13 fixed
     * mask bits stay as captured - same rule as KAT's own Fields::next(). */
    int32_t mult = furi_hal_subghz_get_rolling_counter_mult();
    uint32_t presses = (mult < 1 || mult > 512) ? 1u : (uint32_t)mult;
    uint16_t counter = (uint16_t)instance->generic.cnt;
    for(uint32_t p = 0; p < presses; p++) counter = (uint16_t)((counter + 1u) & 0x1FFu);

    uint64_t hi64;
    uint8_t lo8_out;
    suzuki_ts002_build(head, button, instance->generic.serial, counter, fixed, &hi64, &lo8_out);

    instance->encoder.size_upload = 0;
    instance->overflow = false;
    suzuki_ts002_transmit(instance, hi64, lo8_out);
    if(instance->overflow) return SubGhzProtocolStatusErrorEncoderGetUpload;

    uint32_t repeat = 1;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "Repeat", &repeat, 1);
    instance->encoder.repeat = repeat;

    /* Persist the advanced frame back into "Key"/"Lo8" - so a second press
     * in the same Transmitter session (deserialize called again without
     * the file being reloaded from disk) continues from here, not from
     * the frame that was originally captured. Same pattern as vag_v1.c. */
    uint8_t key_data[sizeof(uint64_t)] = {0};
    for(size_t i = 0; i < sizeof(uint64_t); i++) {
        key_data[sizeof(uint64_t) - i - 1] = (uint8_t)((hi64 >> (i * 8)) & 0xFFu);
    }
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(flipper_format, "Key", key_data, sizeof(key_data)))
        return SubGhzProtocolStatusErrorParserOthers;
    uint32_t lo8_save = lo8_out;
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_uint32(flipper_format, "Lo8", &lo8_save, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    suzuki_ts002_fields(&instance->generic, hi64, lo8_out);
    subghz_block_generic_global.current_cnt = instance->generic.cnt;
    subghz_block_generic_global.current_btn = instance->generic.btn;

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
