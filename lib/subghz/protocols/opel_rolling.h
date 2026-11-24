#pragma once

#include "base.h"

// Opel Rolling - an old two-button Opel remote key: 60-bit period-coded
// frame (button, 24-bit serial, an obfuscated 24-bit plain value holding a
// 12-bit press counter, a rotating-nibble checksum). Decode + encode (the
// "next press" generator below, same as KAT's own - see opel_rolling.c);
// KAT's separate 32-bit "button held" frame is still left out, decode only
// for that one. Not our opel_v0.c or opel_v1.c.
#define SUBGHZ_PROTOCOL_OPEL_ROLLING_NAME "Opel Rolling"

typedef struct SubGhzProtocolDecoderOpelRolling SubGhzProtocolDecoderOpelRolling;
typedef struct SubGhzProtocolEncoderOpelRolling SubGhzProtocolEncoderOpelRolling;

extern const SubGhzProtocolDecoder subghz_protocol_opel_rolling_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_opel_rolling_encoder;
extern const SubGhzProtocol subghz_protocol_opel_rolling;

void* subghz_protocol_decoder_opel_rolling_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_opel_rolling_free(void* context);
void subghz_protocol_decoder_opel_rolling_reset(void* context);
void subghz_protocol_decoder_opel_rolling_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_opel_rolling_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_opel_rolling_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_opel_rolling_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_opel_rolling_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_opel_rolling_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_opel_rolling_free(void* context);
void subghz_protocol_encoder_opel_rolling_stop(void* context);
LevelDuration subghz_protocol_encoder_opel_rolling_yield(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_opel_rolling_deserialize(void* context, FlipperFormat* flipper_format);
