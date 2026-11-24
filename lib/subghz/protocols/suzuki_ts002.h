#pragma once

#include "base.h"

// Suzuki TS002 - Calsonic Kansei's two-button remote key for Suzuki:
// 72-bit frame (fixed head byte, button, 24-bit ID, a 32-bit "code" that
// mixes the press counter with the rotated/masked ID - not a real cipher,
// but a deliberate bit-shuffle KAT worked out in full). Decode + encode
// (KAT's own "next press" rule - see suzuki_ts002.c). Not our suzuki.c.
#define SUBGHZ_PROTOCOL_SUZUKI_TS002_NAME "Suzuki TS002"

typedef struct SubGhzProtocolDecoderSuzukiTs002 SubGhzProtocolDecoderSuzukiTs002;
typedef struct SubGhzProtocolEncoderSuzukiTs002 SubGhzProtocolEncoderSuzukiTs002;

extern const SubGhzProtocolDecoder subghz_protocol_suzuki_ts002_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_suzuki_ts002_encoder;
extern const SubGhzProtocol subghz_protocol_suzuki_ts002;

void* subghz_protocol_decoder_suzuki_ts002_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_suzuki_ts002_free(void* context);
void subghz_protocol_decoder_suzuki_ts002_reset(void* context);
void subghz_protocol_decoder_suzuki_ts002_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_suzuki_ts002_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_suzuki_ts002_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_suzuki_ts002_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_suzuki_ts002_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_suzuki_ts002_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_suzuki_ts002_free(void* context);
void subghz_protocol_encoder_suzuki_ts002_stop(void* context);
LevelDuration subghz_protocol_encoder_suzuki_ts002_yield(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_suzuki_ts002_deserialize(
    void* context, FlipperFormat* flipper_format);
