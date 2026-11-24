#pragma once

#include "base.h"

// Microchip HCS361 KeeLoq code-hopping encoder chip - 67-bit frame (32-bit
// encrypted hop, 28-bit serial, 4-bit button, VLOW, 2-bit CRC). Decode
// only, and only the chip's "Sync, 833 bit/s" PWM timing (the most common
// one on the road - Jeep/Chrysler remotes); the chip supports 5 other
// EEPROM-selectable timings this file does not decode. See
// microchip_hcs361.c for the source.
#define SUBGHZ_PROTOCOL_MICROCHIP_HCS361_NAME "Microchip-HCS361"

typedef struct SubGhzProtocolDecoderMicrochipHcs361 SubGhzProtocolDecoderMicrochipHcs361;

extern const SubGhzProtocolDecoder subghz_protocol_microchip_hcs361_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_microchip_hcs361_encoder;
extern const SubGhzProtocol subghz_protocol_microchip_hcs361;

void* subghz_protocol_decoder_microchip_hcs361_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_microchip_hcs361_free(void* context);
void subghz_protocol_decoder_microchip_hcs361_reset(void* context);
void subghz_protocol_decoder_microchip_hcs361_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_microchip_hcs361_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_microchip_hcs361_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_microchip_hcs361_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_microchip_hcs361_get_string(void* context, FuriString* output);
