#pragma once

#include "base.h"

// EXPERIMENTAL / UNVALIDATED - see mazda_v3.c for details.
#define SUBGHZ_PROTOCOL_MAZDA_V3_NAME "Mazda V3 (unverified)"

typedef struct SubGhzProtocolDecoderMazdaV3 SubGhzProtocolDecoderMazdaV3;

extern const SubGhzProtocolDecoder subghz_protocol_mazda_v3_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_mazda_v3_encoder;
extern const SubGhzProtocol subghz_protocol_mazda_v3;

void* subghz_protocol_decoder_mazda_v3_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_mazda_v3_free(void* context);
void subghz_protocol_decoder_mazda_v3_reset(void* context);
void subghz_protocol_decoder_mazda_v3_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_mazda_v3_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_mazda_v3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_mazda_v3_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_mazda_v3_get_string(void* context, FuriString* output);
