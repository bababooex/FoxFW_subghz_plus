#pragma once

#include "base.h"

<<<<<<< HEAD
// Proteco MOVER gate remotes - 24-bit fixed code (22-bit serial + 2-bit
// button). See proteco.c for the source and timing.
=======
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
#define SUBGHZ_PROTOCOL_PROTECO_NAME "Proteco"

typedef struct SubGhzProtocolDecoderProteco SubGhzProtocolDecoderProteco;
typedef struct SubGhzProtocolEncoderProteco SubGhzProtocolEncoderProteco;

extern const SubGhzProtocolDecoder subghz_protocol_proteco_decoder;
<<<<<<< HEAD
extern const SubGhzProtocolEncoder subghz_protocol_proteco_encoder;
extern const SubGhzProtocol subghz_protocol_proteco;

void* subghz_protocol_decoder_proteco_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_proteco_free(void* context);
void subghz_protocol_decoder_proteco_reset(void* context);
void subghz_protocol_decoder_proteco_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_proteco_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_proteco_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_proteco_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_proteco_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_proteco_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_proteco_free(void* context);
void subghz_protocol_encoder_proteco_stop(void* context);
LevelDuration subghz_protocol_encoder_proteco_yield(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_proteco_deserialize(void* context, FlipperFormat* flipper_format);
=======
extern const SubGhzProtocol subghz_protocol_proteco;

void* subghz_protocol_decoder_proteco_alloc(SubGhzEnvironment* environment);

void subghz_protocol_decoder_proteco_feed(void* context, bool level, uint32_t duration);

SubGhzProtocolStatus
    subghz_protocol_decoder_proteco_deserialize(void* context, FlipperFormat* flipper_format);

void subghz_protocol_decoder_proteco_get_string(void* context, FuriString* output);
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
