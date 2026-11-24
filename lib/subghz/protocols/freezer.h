#pragma once

#include "base.h"

// Freezer - 24-bit fixed code (20-bit serial + 4-bit button). See
// freezer.c for the source and timing.
#define SUBGHZ_PROTOCOL_FREEZER_NAME "Freezer"

typedef struct SubGhzProtocolDecoderFreezer SubGhzProtocolDecoderFreezer;
typedef struct SubGhzProtocolEncoderFreezer SubGhzProtocolEncoderFreezer;

extern const SubGhzProtocolDecoder subghz_protocol_freezer_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_freezer_encoder;
extern const SubGhzProtocol subghz_protocol_freezer;

void* subghz_protocol_decoder_freezer_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_freezer_free(void* context);
void subghz_protocol_decoder_freezer_reset(void* context);
void subghz_protocol_decoder_freezer_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_freezer_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_freezer_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_freezer_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_freezer_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_freezer_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_freezer_free(void* context);
void subghz_protocol_encoder_freezer_stop(void* context);
LevelDuration subghz_protocol_encoder_freezer_yield(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_freezer_deserialize(void* context, FlipperFormat* flipper_format);
