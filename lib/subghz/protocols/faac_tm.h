#pragma once

#include "base.h"

// Faac TM - 12-bit DIP-switch fixed code (FAAC TM gate remotes). See
// faac_tm.c for the source and timing.
#define SUBGHZ_PROTOCOL_FAAC_TM_NAME "Faac TM"

typedef struct SubGhzProtocolDecoderFaacTm SubGhzProtocolDecoderFaacTm;
typedef struct SubGhzProtocolEncoderFaacTm SubGhzProtocolEncoderFaacTm;

extern const SubGhzProtocolDecoder subghz_protocol_faac_tm_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_faac_tm_encoder;
extern const SubGhzProtocol subghz_protocol_faac_tm;

void* subghz_protocol_decoder_faac_tm_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_faac_tm_free(void* context);
void subghz_protocol_decoder_faac_tm_reset(void* context);
void subghz_protocol_decoder_faac_tm_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_faac_tm_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_faac_tm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_faac_tm_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_faac_tm_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_faac_tm_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_faac_tm_free(void* context);
void subghz_protocol_encoder_faac_tm_stop(void* context);
LevelDuration subghz_protocol_encoder_faac_tm_yield(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_faac_tm_deserialize(void* context, FlipperFormat* flipper_format);
