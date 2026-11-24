#pragma once

#include "base.h"

// Cardin S466-TX2 garage-door remote - 24-bit fixed code (9 tri-state DIP
// switches + button). Decode only. See cardin_s466.c for the source.
#define SUBGHZ_PROTOCOL_CARDIN_S466_NAME "Cardin-S466"

typedef struct SubGhzProtocolDecoderCardinS466 SubGhzProtocolDecoderCardinS466;

extern const SubGhzProtocolDecoder subghz_protocol_cardin_s466_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_cardin_s466_encoder;
extern const SubGhzProtocol subghz_protocol_cardin_s466;

void* subghz_protocol_decoder_cardin_s466_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_cardin_s466_free(void* context);
void subghz_protocol_decoder_cardin_s466_reset(void* context);
void subghz_protocol_decoder_cardin_s466_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_cardin_s466_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_cardin_s466_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_cardin_s466_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_cardin_s466_get_string(void* context, FuriString* output);
