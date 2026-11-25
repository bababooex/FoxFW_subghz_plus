#include "microchip_hcs361.h"
<<<<<<< HEAD
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <stdlib.h>

/*
 * Microchip HCS361 KeeLoq code-hopping encoder chip, DECODE ONLY. Ported
 * from rtl_433's src/devices/hcs361.c (GPL-2.0), cross-checked against the
 * KAT project's port (github.com/KaraZajac/KAT, src/protocols/
 * microchip_hcs361.rs). No Flipper reference has this one.
 *
 * The chip is EEPROM-configurable into 6 different timings/line codings
 * (plain PWM with or without a sync pulse at 833 or 1667 bit/s, or
 * PIWM_DC at 2500/5000 bit/s). This file implements ONLY the single most
 * common one on the road - "Sync, 833 bit/s" PWM (the Jeep/Chrysler
 * remotes in rtl_433's own test recordings) - by deliberate scope choice,
 * not oversight: the other 5 would each need their own state machine.
 *
 * Air format (AM/OOK, 833 bit/s, Te=400us): a sync mark of 10*Te (4000us),
 * then 67 bits, each bit's value is its own HIGH (mark) width alone - short
 * (~400us) = bit 1, long (~800us) = bit 0; the LOW that follows carries no
 * information and isn't checked (same convention as cardin_s466.c /
 * chrysler_car_remote.c).
 *
 * Frame (67 bits, LSB-first fields): 32-bit encrypted "hop", 28-bit serial
 * + 4-bit button packed into the next 32 bits, 1 battery-low bit, then a
 * 2-bit CRC (the datasheet's shift-register CRC over the first 65 bits;
 * the last bit may also be read with the battery-low flag's effect on it
 * undone, per the datasheet's own note, so both are tried).
 *
 * The 32-bit "hop" is what an HCS361 chip encrypts with KeeLoq using
 * whatever manufacturer key the remote's maker programmed into it - this
 * file does NOT attempt that decryption (unlike this firmware's own
 * keeloq.c, which tries its manufacturer-key list against normal
 * automotive KeeLoq captures): doing that properly for this chip's frame
 * shape was out of scope here. The raw hop is shown in hex instead, along
 * with the serial and button, which already confirms the signal and
 * identifies the remote.
 */

#define HCS361_BITS 67u
#define HCS361_TE_SHORT 400u
#define HCS361_TE_LONG 800u
#define HCS361_TE_DELTA 140u
#define HCS361_SYNC_NOM 4000u
#define HCS361_SYNC_DELTA 1400u

#define HCS361_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) < HCS361_TE_DELTA)

typedef enum {
    MicrochipHcs361StepIdle = 0,
    MicrochipHcs361StepData,
} MicrochipHcs361DecoderStep;

struct SubGhzProtocolDecoderMicrochipHcs361 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint32_t hop;
    uint32_t fix;
    uint8_t tail; /* bit0 VLOW, bit1 CRC hi, bit2 CRC lo */
    uint8_t count;
};

const SubGhzProtocolDecoder subghz_protocol_microchip_hcs361_decoder = {
    .alloc = subghz_protocol_decoder_microchip_hcs361_alloc,
    .free = subghz_protocol_decoder_microchip_hcs361_free,
    .reset = subghz_protocol_decoder_microchip_hcs361_reset,
    .feed = subghz_protocol_decoder_microchip_hcs361_feed,
    .get_hash_data = subghz_protocol_decoder_microchip_hcs361_get_hash_data,
    .serialize = subghz_protocol_decoder_microchip_hcs361_serialize,
    .deserialize = subghz_protocol_decoder_microchip_hcs361_deserialize,
    .get_string = subghz_protocol_decoder_microchip_hcs361_get_string,
};

/* Decode-only, as rtl_433 (no manufacturer-key decryption attempted here -
 * see the file header). */
const SubGhzProtocolEncoder subghz_protocol_microchip_hcs361_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_microchip_hcs361 = {
    .name = SUBGHZ_PROTOCOL_MICROCHIP_HCS361_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_microchip_hcs361_decoder,
    .encoder = &subghz_protocol_microchip_hcs361_encoder,
};

static inline int hcs361_bit(uint32_t hop, uint32_t fix, uint8_t tail, int i) {
    if(i < 32) return (int)((hop >> i) & 1u);
    if(i < 64) return (int)((fix >> (i - 32)) & 1u);
    return (int)((tail >> (i - 64)) & 1u);
}

/* The datasheet's 2-bit shift-register CRC over the first 65 bits. When
 * flip_bit64 is set, bit 64 (VLOW) is read inverted first - the datasheet's
 * own documented allowance for a battery-low flag flipped after the CRC was
 * computed. */
static int hcs361_crc(uint32_t hop, uint32_t fix, uint8_t tail, bool flip_bit64) {
    int32_t crc = 0;
    for(int i = 0; i < 65; i++) {
        int bit = hcs361_bit(hop, fix, tail, i);
        if(i == 64 && flip_bit64) bit = !bit;
        int crc_bit = ((crc >> 1) ^ bit) & 1;
        crc = crc_bit | (((crc_bit ^ crc) << 1) & 2);
    }
    return crc;
}

static bool hcs361_fields(SubGhzBlockGeneric* generic, uint32_t hop, uint32_t fix, uint8_t tail) {
    if(hop == 0xFFFFFFFFu && fix == 0xFFFFFFFFu) return false; /* implausible: all-ones */
    int actual_crc = (hcs361_bit(hop, fix, tail, 65) << 1) | hcs361_bit(hop, fix, tail, 66);
    if(actual_crc != hcs361_crc(hop, fix, tail, false) &&
       actual_crc != hcs361_crc(hop, fix, tail, true))
        return false;
    uint32_t serial = fix & 0x0FFFFFFFu;
    if(serial == 0) return false;

    generic->data = ((uint64_t)fix << 32) | hop;
    generic->data_count_bit = 64; /* the 3-bit VLOW/CRC tail is not persisted - see get_string */
    generic->serial = serial;
    generic->btn = (uint8_t)(fix >> 28);
    return true;
}

void* subghz_protocol_decoder_microchip_hcs361_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderMicrochipHcs361* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_microchip_hcs361;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_MICROCHIP_HCS361_NAME;
    return instance;
}

void subghz_protocol_decoder_microchip_hcs361_free(void* context) {
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

#define TAG "HCS361"

#define HCS361_FRAME_BITS 67
#define HCS361_PRE_MIN    5
#define HCS361_TE         200U
#define HCS361_TE_DELTA   90U
#define HCS361_HDR_MIN    1200U
#define HCS361_END_GAP    800U

static const SubGhzBlockConst subghz_protocol_hcs361_const = {
    .te_short = HCS361_TE,
    .te_long = 2 * HCS361_TE,
    .te_delta = HCS361_TE_DELTA,
    .min_count_bit_for_found = 64,
};

typedef enum {
    HCS361StepReset = 0,
    HCS361StepPreamble,
    HCS361StepSaveDuration,
    HCS361StepCheckDuration,
} HCS361Step;

struct SubGhzProtocolDecoderHCS361 {
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
SUBGHZ_ASSERT_DECODER_COMMON_LAYOUT(SubGhzProtocolDecoderHCS361);

static inline uint8_t hcs361_bit(const uint8_t* b, int i) {
    return (b[i >> 3] >> (7 - (i & 7))) & 1;
}

static bool hcs361_crc_ok(const uint8_t* b, uint8_t actual_crc) {
    int crc = 0, crc_bat_low = 0;
    for(int i = 0; i < 65; i++) {
        int bit = hcs361_bit(b, i);
        int crc_bit = ((crc >> 1) ^ bit) & 0x1;
        if(i == 64) {
            int crc_bit_bl = crc_bit ^ 1;
            crc_bat_low = crc_bit_bl | (((crc_bit_bl ^ crc) << 1) & 0x2);
        }
        crc = crc_bit | (((crc_bit ^ crc) << 1) & 0x2);
    }
    return actual_crc == crc || actual_crc == crc_bat_low;
}

static void hcs361_add_bit(SubGhzProtocolDecoderHCS361* instance, uint8_t bit) {
    if(instance->bit_count < HCS361_FRAME_BITS) {
        int i = instance->bit_count;
        if(bit) instance->frame[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        instance->bit_count++;
    }
}

static void hcs361_process(SubGhzProtocolDecoderHCS361* instance) {
    if(instance->bit_count != HCS361_FRAME_BITS) return;
    const uint8_t* b = instance->frame;

    bool all_ff = true;
    for(int i = 0; i < 8; i++)
        if(b[i] != 0xFF) {
            all_ff = false;
            break;
        }
    if(all_ff) return;

    uint8_t actual_crc = (uint8_t)((b[8] >> 5) & 0x3);
    if(!hcs361_crc_ok(b, actual_crc)) return;

    uint64_t cw = 0;
    for(int i = 0; i < 64; i++) cw = (cw << 1) | hcs361_bit(b, i);

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

void* subghz_protocol_decoder_hcs361_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolDecoderHCS361* instance = calloc(1, sizeof(SubGhzProtocolDecoderHCS361));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_hcs361;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->keystore = subghz_environment_get_keystore(environment);
    return instance;
}

void subghz_protocol_decoder_hcs361_free(void* context) {
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
    furi_check(context);
    free(context);
}

<<<<<<< HEAD
void subghz_protocol_decoder_microchip_hcs361_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMicrochipHcs361* instance = context;
    instance->decoder.parser_step = MicrochipHcs361StepIdle;
    instance->hop = 0;
    instance->fix = 0;
    instance->tail = 0;
    instance->count = 0;
}

void subghz_protocol_decoder_microchip_hcs361_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderMicrochipHcs361* instance = context;

    /* The low (space) between pulses carries no information in this scheme
     * - only mark widths are classified (see the file header). */
    if(!level) return;

    if(DURATION_DIFF(duration, HCS361_SYNC_NOM) < HCS361_SYNC_DELTA) {
        instance->hop = 0;
        instance->fix = 0;
        instance->tail = 0;
        instance->count = 0;
        instance->decoder.parser_step = MicrochipHcs361StepData;
        return;
    }

    if(instance->decoder.parser_step != MicrochipHcs361StepData) return;

    int bit;
    if(HCS361_IN_TOL(duration, HCS361_TE_SHORT)) {
        bit = 1;
    } else if(HCS361_IN_TOL(duration, HCS361_TE_LONG)) {
        bit = 0;
    } else {
        instance->decoder.parser_step = MicrochipHcs361StepIdle;
        return;
    }

    uint8_t i = instance->count;
    if(i < 32)
        instance->hop |= (uint32_t)bit << i;
    else if(i < 64)
        instance->fix |= (uint32_t)bit << (i - 32);
    else
        instance->tail |= (uint8_t)(bit << (i - 64));
    instance->count++;

    if(instance->count == HCS361_BITS) {
        bool valid =
            hcs361_fields(&instance->generic, instance->hop, instance->fix, instance->tail);
        instance->decoder.parser_step = MicrochipHcs361StepIdle;
        if(valid && instance->base.callback)
            instance->base.callback(&instance->base, instance->base.context);
    }
}

uint8_t subghz_protocol_decoder_microchip_hcs361_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMicrochipHcs361* instance = context;
    uint8_t hash = 0;
    hash = (hash * 33u) ^ (uint8_t)(instance->hop & 0xFFu);
    hash = (hash * 33u) ^ (uint8_t)(instance->fix & 0xFFu);
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_microchip_hcs361_serialize(
=======
void subghz_protocol_decoder_hcs361_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHCS361* instance = context;
    instance->step = HCS361StepReset;
    instance->pre_count = 0;
    instance->bit_count = 0;
    memset(instance->frame, 0, sizeof(instance->frame));
}

void subghz_protocol_decoder_hcs361_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderHCS361* instance = context;
    const uint32_t te = HCS361_TE;
    const uint32_t d = HCS361_TE_DELTA;

    switch(instance->step) {
    case HCS361StepReset:
        if(level && DURATION_DIFF(duration, te) < d) {
            instance->step = HCS361StepPreamble;
            instance->pre_count = 1;
        }
        break;

    case HCS361StepPreamble:
        if(!level) {
            if(duration >= HCS361_HDR_MIN && instance->pre_count >= HCS361_PRE_MIN) {

                instance->step = HCS361StepSaveDuration;
                instance->bit_count = 0;
                memset(instance->frame, 0, sizeof(instance->frame));
            } else if(DURATION_DIFF(duration, te) < d) {

            } else {
                instance->step = HCS361StepReset;
                instance->pre_count = 0;
            }
        } else {
            if(DURATION_DIFF(duration, te) < d) {
                if(instance->pre_count < 0xFFFF) instance->pre_count++;
            } else if(duration >= HCS361_HDR_MIN) {

            } else {
                instance->step = HCS361StepReset;
                instance->pre_count = 0;
            }
        }
        break;

    case HCS361StepSaveDuration:
        if(level) {
            instance->te_last = duration;
            instance->step = HCS361StepCheckDuration;
        } else if(duration >= HCS361_END_GAP) {
            instance->step = HCS361StepReset;
            instance->pre_count = 0;
        }
        break;

    case HCS361StepCheckDuration:
        if(!level) {
            if(duration >= HCS361_END_GAP) {
                hcs361_process(instance);
                instance->step = HCS361StepReset;
                instance->pre_count = 0;
            } else if(
                DURATION_DIFF(instance->te_last, te) < d && DURATION_DIFF(duration, 2 * te) < 2 * d) {
                hcs361_add_bit(instance, 1);
                instance->step = HCS361StepSaveDuration;
            } else if(
                DURATION_DIFF(instance->te_last, 2 * te) < 2 * d && DURATION_DIFF(duration, te) < d) {
                hcs361_add_bit(instance, 0);
                instance->step = HCS361StepSaveDuration;
            } else {
                instance->step = HCS361StepReset;
                instance->pre_count = 0;
            }
        } else {
            instance->step = HCS361StepReset;
            instance->pre_count = 0;
        }
        break;
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_hcs361_serialize(
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
<<<<<<< HEAD
    SubGhzProtocolDecoderMicrochipHcs361* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_microchip_hcs361_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderMicrochipHcs361* instance = context;
    /* The 3-bit VLOW/CRC tail isn't persisted (see hcs361_fields), so a
     * loaded key can't be CRC-rechecked here - it was already checked once,
     * at receive time, before it could ever be saved. */
    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64);
    if(status != SubGhzProtocolStatusOk) return status;
    instance->hop = (uint32_t)instance->generic.data;
    instance->fix = (uint32_t)(instance->generic.data >> 32);
    instance->generic.serial = instance->fix & 0x0FFFFFFFu;
    instance->generic.btn = (uint8_t)(instance->fix >> 28);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_microchip_hcs361_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderMicrochipHcs361* instance = context;
    furi_string_cat_printf(
        output,
        "%s 67bit\r\n"
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
    SubGhzProtocolDecoderHCS361* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;
    if(!flipper_format_write_hex(flipper_format, "Frame", instance->frame, 9)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_hcs361_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderHCS361* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_hcs361_const.min_count_bit_for_found);
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

void subghz_protocol_decoder_hcs361_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderHCS361* instance = context;
    const SubGhzDecodeInput input = {
        .generic = &instance->generic,
        .aux = instance->manufacture_name,
    };
    subghz_decode_plugin_get_string("Microchip-HCS361", &input, output);
}

const SubGhzProtocolDecoder subghz_protocol_hcs361_decoder = {
    .alloc = subghz_protocol_decoder_hcs361_alloc,
    .free = subghz_protocol_decoder_hcs361_free,
    .feed = subghz_protocol_decoder_hcs361_feed,
    .reset = subghz_protocol_decoder_hcs361_reset,
    .get_hash_data = subghz_protocol_decoder_common_get_hash_data,
    .serialize = subghz_protocol_decoder_hcs361_serialize,
    .deserialize = subghz_protocol_decoder_hcs361_deserialize,
    .get_string = subghz_protocol_decoder_hcs361_get_string,
};

static const SubGhzEncoderExternal subghz_protocol_hcs361_encoder_ext =
    SUBGHZ_ENCODER_EXTERNAL("enc_recon_pwm");

const SubGhzProtocol subghz_protocol_hcs361 = {
    .name = SUBGHZ_PROTOCOL_HCS361_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_hcs361_decoder,
    .encoder = &subghz_protocol_hcs361_encoder_ext.vtable,
};
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
