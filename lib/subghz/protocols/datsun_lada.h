#pragma once
<<<<<<< HEAD

#include "base.h"

#define SUBGHZ_PROTOCOL_DATSUN_LADA_NAME "Datsun/Lada"
=======
#include "base.h"

#define SUBGHZ_PROTOCOL_DATSUN_LADA_NAME "Datsun Lada"
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5

typedef struct SubGhzProtocolDecoderDatsunLada SubGhzProtocolDecoderDatsunLada;

extern const SubGhzProtocolDecoder subghz_protocol_datsun_lada_decoder;
<<<<<<< HEAD
extern const SubGhzProtocolEncoder subghz_protocol_datsun_lada_encoder;
=======
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
extern const SubGhzProtocol subghz_protocol_datsun_lada;

void* subghz_protocol_decoder_datsun_lada_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_datsun_lada_free(void* context);
void subghz_protocol_decoder_datsun_lada_reset(void* context);
void subghz_protocol_decoder_datsun_lada_feed(void* context, bool level, uint32_t duration);
<<<<<<< HEAD
uint8_t subghz_protocol_decoder_datsun_lada_get_hash_data(void* context);
=======
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
SubGhzProtocolStatus subghz_protocol_decoder_datsun_lada_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_datsun_lada_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_datsun_lada_get_string(void* context, FuriString* output);
<<<<<<< HEAD

void* subghz_protocol_encoder_datsun_lada_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_datsun_lada_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_datsun_lada_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_datsun_lada_stop(void* context);
LevelDuration subghz_protocol_encoder_datsun_lada_yield(void* context);
=======
>>>>>>> 6b25e408906383d0bd28b07be4ee59e66d56a8a5
