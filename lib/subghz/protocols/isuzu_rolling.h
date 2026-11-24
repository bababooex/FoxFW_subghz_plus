#pragma once

#include "base.h"

// Isuzu Rolling - an Isuzu remote sending a 105-bit differential Manchester
// frame, 8 copies a press with a real ~20ms gap between them. Decode only
// - cipher unknown, no checksum found, model/year not known. See
// isuzu_rolling.c for the source.
#define SUBGHZ_PROTOCOL_ISUZU_ROLLING_NAME "Isuzu Rolling"

typedef struct SubGhzProtocolDecoderIsuzuRolling SubGhzProtocolDecoderIsuzuRolling;

extern const SubGhzProtocolDecoder subghz_protocol_isuzu_rolling_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_isuzu_rolling_encoder;
extern const SubGhzProtocol subghz_protocol_isuzu_rolling;

void* subghz_protocol_decoder_isuzu_rolling_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_isuzu_rolling_free(void* context);
void subghz_protocol_decoder_isuzu_rolling_reset(void* context);
void subghz_protocol_decoder_isuzu_rolling_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_isuzu_rolling_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_isuzu_rolling_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_isuzu_rolling_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_isuzu_rolling_get_string(void* context, FuriString* output);
