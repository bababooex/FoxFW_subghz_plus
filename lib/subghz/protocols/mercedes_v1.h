#pragma once

#include "base.h"

// Mercedes V1: a second Mercedes-Benz rolling-code frame, distinct from this tree's own
// mercedes.c (Mercedes V0, 112-bit) - different timing (50/100us vs 500/1000us) and a wider
// 122-bit frame. Decode + encode: the next counter value is sent, the cipher-unknown "hop"
// payload is carried forward unchanged (same approach as this session's other
// cipher-unknown rolling protocols, e.g. Opel Rolling/Suzuki TS002).
#define SUBGHZ_PROTOCOL_MERCEDES_V1_NAME "Mercedes V1"

typedef struct SubGhzProtocolDecoderMercedesV1 SubGhzProtocolDecoderMercedesV1;
typedef struct SubGhzProtocolEncoderMercedesV1 SubGhzProtocolEncoderMercedesV1;

extern const SubGhzProtocolDecoder subghz_protocol_mercedes_v1_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_mercedes_v1_encoder;
extern const SubGhzProtocol subghz_protocol_mercedes_v1;

void* subghz_protocol_decoder_mercedes_v1_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_mercedes_v1_free(void* context);
void subghz_protocol_decoder_mercedes_v1_reset(void* context);
void subghz_protocol_decoder_mercedes_v1_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_mercedes_v1_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_mercedes_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_mercedes_v1_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_mercedes_v1_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_mercedes_v1_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_mercedes_v1_free(void* context);
void subghz_protocol_encoder_mercedes_v1_stop(void* context);
LevelDuration subghz_protocol_encoder_mercedes_v1_yield(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_mercedes_v1_deserialize(
    void* context,
    FlipperFormat* flipper_format);
