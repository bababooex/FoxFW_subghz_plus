#pragma once

#include "base.h"

// Lexus UX - the smart key of a 2021 Lexus UX 250: 256-bit differential
// Manchester frame (fixed head, button, 128-bit rolling code of unknown
// cipher, ID, CRC-16), sent 4x back to back with no gap. Decode only. Not
// the same decode as our lexus_v0.c (which reads the first 224 bits of
// the same raw signal without the differential step - see lexus_ux.c).
#define SUBGHZ_PROTOCOL_LEXUS_UX_NAME "Lexus UX"

typedef struct SubGhzProtocolDecoderLexusUx SubGhzProtocolDecoderLexusUx;

extern const SubGhzProtocolDecoder subghz_protocol_lexus_ux_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_lexus_ux_encoder;
extern const SubGhzProtocol subghz_protocol_lexus_ux;

void* subghz_protocol_decoder_lexus_ux_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_lexus_ux_free(void* context);
void subghz_protocol_decoder_lexus_ux_reset(void* context);
void subghz_protocol_decoder_lexus_ux_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_lexus_ux_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_lexus_ux_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_lexus_ux_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_lexus_ux_get_string(void* context, FuriString* output);
