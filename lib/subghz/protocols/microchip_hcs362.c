#include "microchip_hcs362.h"
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
    furi_check(context);
    free(context);
}

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
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
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
