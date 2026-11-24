#pragma once

#include "base.h"

// Siemens 5WY72XX car remote (DaimlerChrysler SKREEK CS/RS platforms) -
// 96-bit fixed-field frame (32-bit ID, button flags, 16-bit counter, 32-bit
// encrypted part, XOR check). Decode only. See siemens_5wy72xx.c for the
// source. Not the same protocol as our siemens_vdo.c.
#define SUBGHZ_PROTOCOL_SIEMENS_5WY72XX_NAME "Siemens-5WY72XX"

typedef struct SubGhzProtocolDecoderSiemens5wy72xx SubGhzProtocolDecoderSiemens5wy72xx;

extern const SubGhzProtocolDecoder subghz_protocol_siemens_5wy72xx_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_siemens_5wy72xx_encoder;
extern const SubGhzProtocol subghz_protocol_siemens_5wy72xx;

void* subghz_protocol_decoder_siemens_5wy72xx_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_siemens_5wy72xx_free(void* context);
void subghz_protocol_decoder_siemens_5wy72xx_reset(void* context);
void subghz_protocol_decoder_siemens_5wy72xx_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_siemens_5wy72xx_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_siemens_5wy72xx_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_siemens_5wy72xx_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_siemens_5wy72xx_get_string(void* context, FuriString* output);
