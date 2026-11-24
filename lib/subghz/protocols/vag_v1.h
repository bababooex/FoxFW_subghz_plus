#pragma once

#include "base.h"

// VW-1: the oldest Volkswagen Group rolling-code remote (VW/Audi/Seat/Skoda,
// ~1998-2005) - see vag_v1.c for the scheme and its source.
#define SUBGHZ_PROTOCOL_VAG_V1_NAME "VAG V1 (VW-1 Legacy)"

typedef struct SubGhzProtocolDecoderVagV1 SubGhzProtocolDecoderVagV1;
typedef struct SubGhzProtocolEncoderVagV1 SubGhzProtocolEncoderVagV1;

extern const SubGhzProtocolDecoder subghz_protocol_vag_v1_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_vag_v1_encoder;
extern const SubGhzProtocol subghz_protocol_vag_v1;

void* subghz_protocol_decoder_vag_v1_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_vag_v1_free(void* context);
void subghz_protocol_decoder_vag_v1_reset(void* context);
void subghz_protocol_decoder_vag_v1_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_vag_v1_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_vag_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_vag_v1_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_vag_v1_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_vag_v1_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_vag_v1_free(void* context);
void subghz_protocol_encoder_vag_v1_stop(void* context);
LevelDuration subghz_protocol_encoder_vag_v1_yield(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_vag_v1_deserialize(void* context, FlipperFormat* flipper_format);
