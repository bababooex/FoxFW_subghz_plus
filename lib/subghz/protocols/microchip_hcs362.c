#include "microchip_hcs362.h"
<<<<<<< HEAD
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <stdlib.h>

/*
 * Microchip HCS362 KeeLoq code-hopping encoder chip, DECODE ONLY. Ported
 * from rtl_433's src/devices/hcs362.c (GPL-2.0), cross-checked against the
 * KAT project's port (github.com/KaraZajac/KAT, src/protocols/
 * microchip_hcs362.rs). No Flipper reference has this one.
 *
 * The chip supports two EEPROM-selectable line codings, PWM or Manchester
 * ("MC"). This file implements ONLY the PWM one, by deliberate scope
 * choice - MC needs the generic demodulator to auto-measure its own bit
 * period from the preamble, which this simple edge-based decoder doesn't
 * attempt.
 *
 * Air format (AM/OOK PWM, Te=200us): UNLIKE microchip_hcs361.c, this mode
 * has no distinct sync pulse - a bit's value is its own HIGH (mark) width
 * alone (short ~200us = 1, long ~400us = 0, same "mark only" convention as
 * hcs361/cardin_s466/chrysler_car_remote), and the frame is found purely
 * positionally: a 12-bit preamble of all 1s, then 69 data bits. Since there
 * is no marker, this decoder free-runs a bit counter and resyncs itself
 * bit-by-bit on any preamble mismatch (the preamble requirement is
 * self-correcting: real noise essentially never produces 12 bits of 1 by
 * chance at the wrong offset).
 *
 * Frame (69 bits, LSB-first fields): 32-bit encrypted "hop", 28-bit serial
 * + 4-bit button, 1 battery-low bit, a 2-bit CRC (datasheet Equation 3-1),
 * 2 queue bits (not used here).
 *
 * As in microchip_hcs361.c, the encrypted "hop" is shown in hex - this file
 * does not attempt the firmware's own keeloq.c-style manufacturer-key
 * decryption for this chip's frame shape.
 */

#define HCS362_PREAMBLE_BITS 12u
#define HCS362_DATA_BITS 69u
#define HCS362_TOTAL_BITS (HCS362_PREAMBLE_BITS + HCS362_DATA_BITS)
#define HCS362_TE_SHORT 200u
#define HCS362_TE_LONG 400u
#define HCS362_TE_DELTA 50u

#define HCS362_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < HCS362_TE_DELTA)

struct SubGhzProtocolDecoderMicrochipHcs362 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint32_t hop;
    uint32_t fix;
    uint8_t tail; /* bit0 VLOW, bit1 CRC0, bit2 CRC1, bit3 QUEUE0, bit4 QUEUE1 */
    uint8_t count; /* 0..12 = preamble, 12..81 = data */
};

const SubGhzProtocolDecoder subghz_protocol_microchip_hcs362_decoder = {
    .alloc = subghz_protocol_decoder_microchip_hcs362_alloc,
    .free = subghz_protocol_decoder_microchip_hcs362_free,
    .reset = subghz_protocol_decoder_microchip_hcs362_reset,
    .feed = subghz_protocol_decoder_microchip_hcs362_feed,
    .get_hash_data = subghz_protocol_decoder_microchip_hcs362_get_hash_data,
    .serialize = subghz_protocol_decoder_microchip_hcs362_serialize,
    .deserialize = subghz_protocol_decoder_microchip_hcs362_deserialize,
    .get_string = subghz_protocol_decoder_microchip_hcs362_get_string,
};

/* Decode-only, as rtl_433 (no manufacturer-key decryption attempted here -
 * see the file header). */
const SubGhzProtocolEncoder subghz_protocol_microchip_hcs362_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_microchip_hcs362 = {
    .name = SUBGHZ_PROTOCOL_MICROCHIP_HCS362_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_microchip_hcs362_decoder,
    .encoder = &subghz_protocol_microchip_hcs362_encoder,
};

static inline int hcs362_bit(uint32_t hop, uint32_t fix, uint8_t tail, int i) {
    if(i < 32) return (int)((hop >> i) & 1u);
    if(i < 64) return (int)((fix >> (i - 32)) & 1u);
    return (int)((tail >> (i - 64)) & 1u);
}

/* The datasheet's Equation 3-1 CRC over the first 65 bits. */
static int hcs362_crc(uint32_t hop, uint32_t fix, uint8_t tail) {
    int crc0 = 0, crc1 = 0;
    for(int i = 0; i < 65; i++) {
        int d = hcs362_bit(hop, fix, tail, i);
        int next_crc1 = crc0 ^ d;
        int next_crc0 = crc0 ^ d ^ crc1;
        crc0 = next_crc0;
        crc1 = next_crc1;
    }
    return (crc1 << 1) | crc0;
}

static bool hcs362_fields(SubGhzBlockGeneric* generic, uint32_t hop, uint32_t fix, uint8_t tail) {
    if(hop == 0xFFFFFFFFu && fix == 0xFFFFFFFFu) return false; /* implausible: all-ones */
    int actual_crc = hcs362_bit(hop, fix, tail, 65) | (hcs362_bit(hop, fix, tail, 66) << 1);
    if(actual_crc != hcs362_crc(hop, fix, tail)) return false;
    uint32_t serial = fix & 0x0FFFFFFFu;
    if(serial == 0) return false;

    generic->data = ((uint64_t)fix << 32) | hop;
    generic->data_count_bit = 64; /* the 5-bit VLOW/CRC/queue tail is not persisted */
    generic->serial = serial;
    generic->btn = (uint8_t)(fix >> 28);
    return true;
}

void* subghz_protocol_decoder_microchip_hcs362_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderMicrochipHcs362* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_microchip_hcs362;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_MICROCHIP_HCS362_NAME;
    return instance;
}

void subghz_protocol_decoder_microchip_hcs362_free(void* context) {
=======
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include "common.h"
#include "decode_plugin.h"
#include "keeloq.h"
#include "../environment.h"
#include "../encode_plugin.h"

#define TAG "HCS362"

#define HCS362_FRAME_BITS   69
#define HCS362_PRE_MIN      8
#define HCS362_TE           200U
#define HCS362_TE_DELTA     90U
#define HCS362_SEP_MIN      450U
#define HCS362_SEP_MAX      780U
#define HCS362_END_GAP      800U

static const SubGhzBlockConst subghz_protocol_hcs362_const = {
    .te_short = HCS362_TE,
    .te_long = 2 * HCS362_TE,
    .te_delta = HCS362_TE_DELTA,
    .min_count_bit_for_found = 64,
};

typedef enum {
    HCS362StepReset = 0,
    HCS362StepPreamble,
    HCS362StepSaveDuration,
    HCS362StepCheckDuration,
} HCS362Step;

struct SubGhzProtocolDecoderHCS362 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint8_t step;
    uint16_t pre_count;
    uint32_t te_last;
    uint8_t frame[9];
    uint16_t bit_count;

    SubGhzKeystore* keystore;
    const char* manufacture_name;
};
SUBGHZ_ASSERT_DECODER_COMMON_LAYOUT(SubGhzProtocolDecoderHCS362);

static inline uint8_t hcs362_bit(const uint8_t* b, int i) {
    return (b[i >> 3] >> (7 - (i & 7))) & 1;
}

static uint8_t hcs362_crc(const uint8_t* b) {
    uint8_t crc0 = 0, crc1 = 0;
    for(int n = 0; n < 65; n++) {
        uint8_t d = hcs362_bit(b, n);
        uint8_t next_crc1 = crc0 ^ d;
        uint8_t next_crc0 = crc0 ^ d ^ crc1;
        crc0 = next_crc0;
        crc1 = next_crc1;
    }
    return (uint8_t)((crc1 << 1) | crc0);
}

static void hcs362_add_bit(SubGhzProtocolDecoderHCS362* instance, uint8_t bit) {
    if(instance->bit_count < HCS362_FRAME_BITS) {
        int i = instance->bit_count;
        if(bit) instance->frame[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        instance->bit_count++;
    }
}

static void hcs362_process(SubGhzProtocolDecoderHCS362* instance) {
    if(instance->bit_count != HCS362_FRAME_BITS) return;
    const uint8_t* b = instance->frame;

    bool all_ff = true;
    for(int i = 1; i < 8; i++)
        if(b[i] != 0xFF) {
            all_ff = false;
            break;
        }
    if(all_ff) return;

    uint8_t actual_crc = (uint8_t)(((b[8] >> 6) & 1) | (((b[8] >> 5) & 1) << 1));
    if(actual_crc != hcs362_crc(b)) return;

    uint64_t cw = 0;
    for(int i = 0; i < 64; i++) cw = (cw << 1) | hcs362_bit(b, i);

    instance->generic.data = cw;
    instance->generic.data_count_bit = 64;
    instance->manufacture_name = "";
    subghz_protocol_keeloq_identify_codeword(
        &instance->generic, instance->keystore, &instance->manufacture_name);
    if(instance->generic.serial == 0) return;

    instance->decoder.decode_data = cw;
    instance->decoder.decode_count_bit = 64;

    if(instance->base.callback) instance->base.callback(&instance->base, instance->base.context);
}

void* subghz_protocol_decoder_hcs362_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolDecoderHCS362* instance = calloc(1, sizeof(SubGhzProtocolDecoderHCS362));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_hcs362;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->keystore = subghz_environment_get_keystore(environment);
    return instance;
}

void subghz_protocol_decoder_hcs362_free(void* context) {
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
    furi_check(context);
    free(context);
}

<<<<<<< HEAD
void subghz_protocol_decoder_microchip_hcs362_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMicrochipHcs362* instance = context;
    instance->hop = 0;
    instance->fix = 0;
    instance->tail = 0;
    instance->count = 0;
}

void subghz_protocol_decoder_microchip_hcs362_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderMicrochipHcs362* instance = context;

    /* The low (space) between pulses carries no information here either -
     * only mark widths are classified (see the file header). */
    if(!level) return;

    int bit;
    if(HCS362_IN_TOL(duration, HCS362_TE_SHORT)) {
        bit = 1;
    } else if(HCS362_IN_TOL(duration, HCS362_TE_LONG)) {
        bit = 0;
    } else {
        instance->count = 0;
        return;
    }

    if(instance->count < HCS362_PREAMBLE_BITS) {
        if(bit != 1) {
            instance->count = 0;
            return;
        }
        instance->count++;
        if(instance->count == HCS362_PREAMBLE_BITS) {
            instance->hop = 0;
            instance->fix = 0;
            instance->tail = 0;
        }
        return;
    }

    uint8_t i = instance->count - HCS362_PREAMBLE_BITS;
    if(i < 32)
        instance->hop |= (uint32_t)bit << i;
    else if(i < 64)
        instance->fix |= (uint32_t)bit << (i - 32);
    else
        instance->tail |= (uint8_t)(bit << (i - 64));
    instance->count++;

    if(instance->count == HCS362_TOTAL_BITS) {
        bool valid =
            hcs362_fields(&instance->generic, instance->hop, instance->fix, instance->tail);
        instance->count = 0;
        if(valid && instance->base.callback)
            instance->base.callback(&instance->base, instance->base.context);
    }
}

uint8_t subghz_protocol_decoder_microchip_hcs362_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMicrochipHcs362* instance = context;
    uint8_t hash = 0;
    hash = (hash * 33u) ^ (uint8_t)(instance->hop & 0xFFu);
    hash = (hash * 33u) ^ (uint8_t)(instance->fix & 0xFFu);
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_microchip_hcs362_serialize(
=======
void subghz_protocol_decoder_hcs362_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHCS362* instance = context;
    instance->step = HCS362StepReset;
    instance->pre_count = 0;
    instance->bit_count = 0;
    memset(instance->frame, 0, sizeof(instance->frame));
}

void subghz_protocol_decoder_hcs362_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderHCS362* instance = context;
    const uint32_t te = HCS362_TE;
    const uint32_t d = HCS362_TE_DELTA;

    switch(instance->step) {
    case HCS362StepReset:
        if(level && DURATION_DIFF(duration, te) < d) {
            instance->step = HCS362StepPreamble;
            instance->pre_count = 1;
        }
        break;

    case HCS362StepPreamble:
        if(!level) {
            if(duration >= HCS362_SEP_MIN && duration <= HCS362_SEP_MAX &&
               instance->pre_count >= HCS362_PRE_MIN) {

                instance->step = HCS362StepSaveDuration;
                instance->bit_count = 0;
                memset(instance->frame, 0, sizeof(instance->frame));
            } else if(DURATION_DIFF(duration, te) < d) {

            } else {
                instance->step = HCS362StepReset;
                instance->pre_count = 0;
            }
        } else {
            if(DURATION_DIFF(duration, te) < d) {
                if(instance->pre_count < 0xFFFF) instance->pre_count++;
            } else {
                instance->step = HCS362StepReset;
                instance->pre_count = 0;
            }
        }
        break;

    case HCS362StepSaveDuration:
        if(level) {
            instance->te_last = duration;
            instance->step = HCS362StepCheckDuration;
        } else if(duration >= HCS362_END_GAP) {
            instance->step = HCS362StepReset;
            instance->pre_count = 0;
        }
        break;

    case HCS362StepCheckDuration:
        if(!level) {
            if(duration >= HCS362_END_GAP) {
                hcs362_process(instance);
                instance->step = HCS362StepReset;
                instance->pre_count = 0;
            } else if(
                DURATION_DIFF(instance->te_last, te) < d && DURATION_DIFF(duration, 2 * te) < 2 * d) {
                hcs362_add_bit(instance, 1);
                instance->step = HCS362StepSaveDuration;
            } else if(
                DURATION_DIFF(instance->te_last, 2 * te) < 2 * d && DURATION_DIFF(duration, te) < d) {
                hcs362_add_bit(instance, 0);
                instance->step = HCS362StepSaveDuration;
            } else {
                instance->step = HCS362StepReset;
                instance->pre_count = 0;
            }
        } else {
            instance->step = HCS362StepReset;
            instance->pre_count = 0;
        }
        break;
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_hcs362_serialize(
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
<<<<<<< HEAD
    SubGhzProtocolDecoderMicrochipHcs362* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_microchip_hcs362_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderMicrochipHcs362* instance = context;
    /* The 5-bit VLOW/CRC/queue tail isn't persisted, so a loaded key can't
     * be CRC-rechecked here - see hcs362_fields. */
    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
    if(status != SubGhzProtocolStatusOk) return status;
    instance->hop = (uint32_t)instance->generic.data;
    instance->fix = (uint32_t)(instance->generic.data >> 32);
    instance->generic.serial = instance->fix & 0x0FFFFFFFu;
    instance->generic.btn = (uint8_t)(instance->fix >> 28);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_microchip_hcs362_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderMicrochipHcs362* instance = context;
    furi_string_cat_printf(
        output,
        "%s 69bit\r\n"
        "Hop(enc):%08lX\r\n"
        "Sn:%07lX Btn:%01X\r\n"
        "KeeLoq hop - manufacturer key\r\n"
        "not tried, shown encrypted\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->hop,
        (unsigned long)instance->generic.serial,
        instance->generic.btn);
}
=======
    SubGhzProtocolDecoderHCS362* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;
    if(!flipper_format_write_hex(flipper_format, "Frame", instance->frame, 9)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_hcs362_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderHCS362* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_hcs362_const.min_count_bit_for_found);
    if(status != SubGhzProtocolStatusOk) return status;
    flipper_format_rewind(flipper_format);
    flipper_format_read_hex(flipper_format, "Frame", instance->frame, 9);
    instance->manufacture_name = "";
    subghz_protocol_keeloq_identify_codeword(
        &instance->generic, instance->keystore, &instance->manufacture_name);
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = instance->generic.data_count_bit;
    return status;
}

void subghz_protocol_decoder_hcs362_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderHCS362* instance = context;
    const SubGhzDecodeInput input = {
        .generic = &instance->generic,
        .aux = instance->manufacture_name,
    };
    subghz_decode_plugin_get_string("Microchip-HCS362", &input, output);
}

const SubGhzProtocolDecoder subghz_protocol_hcs362_decoder = {
    .alloc = subghz_protocol_decoder_hcs362_alloc,
    .free = subghz_protocol_decoder_hcs362_free,
    .feed = subghz_protocol_decoder_hcs362_feed,
    .reset = subghz_protocol_decoder_hcs362_reset,
    .get_hash_data = subghz_protocol_decoder_common_get_hash_data,
    .serialize = subghz_protocol_decoder_hcs362_serialize,
    .deserialize = subghz_protocol_decoder_hcs362_deserialize,
    .get_string = subghz_protocol_decoder_hcs362_get_string,
};

static const SubGhzEncoderExternal subghz_protocol_hcs362_encoder_ext =
    SUBGHZ_ENCODER_EXTERNAL("enc_recon_pwm");

const SubGhzProtocol subghz_protocol_hcs362 = {
    .name = SUBGHZ_PROTOCOL_HCS362_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_hcs362_decoder,
    .encoder = &subghz_protocol_hcs362_encoder_ext.vtable,
};
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
