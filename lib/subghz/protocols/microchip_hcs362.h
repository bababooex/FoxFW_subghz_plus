#pragma once
<<<<<<< HEAD

#include "base.h"

// Microchip HCS362 KeeLoq code-hopping encoder chip - 69-bit frame (32-bit
// encrypted hop, 28-bit serial, 4-bit button, VLOW, 2-bit CRC, 2-bit
// queue). Decode only, and only the chip's PWM timing (not its Manchester
// "MC" alternative - see microchip_hcs362.c for the source).
#define SUBGHZ_PROTOCOL_MICROCHIP_HCS362_NAME "Microchip-HCS362"

typedef struct SubGhzProtocolDecoderMicrochipHcs362 SubGhzProtocolDecoderMicrochipHcs362;

extern const SubGhzProtocolDecoder subghz_protocol_microchip_hcs362_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_microchip_hcs362_encoder;
extern const SubGhzProtocol subghz_protocol_microchip_hcs362;

void* subghz_protocol_decoder_microchip_hcs362_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_microchip_hcs362_free(void* context);
void subghz_protocol_decoder_microchip_hcs362_reset(void* context);
void subghz_protocol_decoder_microchip_hcs362_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_microchip_hcs362_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_microchip_hcs362_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_microchip_hcs362_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_microchip_hcs362_get_string(void* context, FuriString* output);
=======
#include "base.h"

#define SUBGHZ_PROTOCOL_HCS362_NAME "Microchip-HCS362"

typedef struct SubGhzProtocolDecoderHCS362 SubGhzProtocolDecoderHCS362;

extern const SubGhzProtocolDecoder subghz_protocol_hcs362_decoder;
extern const SubGhzProtocol subghz_protocol_hcs362;

void* subghz_protocol_decoder_hcs362_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_hcs362_free(void* context);
void subghz_protocol_decoder_hcs362_reset(void* context);
void subghz_protocol_decoder_hcs362_feed(void* context, bool level, uint32_t duration);
SubGhzProtocolStatus subghz_protocol_decoder_hcs362_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_hcs362_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_hcs362_get_string(void* context, FuriString* output);
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
