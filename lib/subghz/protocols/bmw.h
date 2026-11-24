#pragma once

#include "base.h"

// BMW (generic rolling-code remote - not our bmw_cas3.c/bmw_cas4.c/bmw_v2.c/bmw_v3.c, which
// are different frames): 128-bit Manchester-chip-paired frame (16-bit sync, 32-bit serial,
// a 2-bit button code, a fixed check byte, a 17-bit counter and a 47-bit "hop" payload whose
// cipher is unknown - carried forward unchanged on re-encode, same as this session's other
// cipher-unknown rolling protocols, e.g. Opel Rolling/Suzuki TS002's captured material).
// Decode + encode: the next counter value is sent, the hop itself can't be regenerated.
#define SUBGHZ_PROTOCOL_BMW_NAME "BMW"

typedef struct SubGhzProtocolDecoderBMW SubGhzProtocolDecoderBMW;
typedef struct SubGhzProtocolEncoderBMW SubGhzProtocolEncoderBMW;

extern const SubGhzProtocolDecoder subghz_protocol_bmw_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_bmw_encoder;
extern const SubGhzProtocol subghz_protocol_bmw;

void* subghz_protocol_decoder_bmw_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_bmw_free(void* context);
void subghz_protocol_decoder_bmw_reset(void* context);
void subghz_protocol_decoder_bmw_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_bmw_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_bmw_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_bmw_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_bmw_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_bmw_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_bmw_free(void* context);
void subghz_protocol_encoder_bmw_stop(void* context);
LevelDuration subghz_protocol_encoder_bmw_yield(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_bmw_deserialize(void* context, FlipperFormat* flipper_format);
