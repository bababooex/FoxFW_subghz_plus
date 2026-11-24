#pragma once

#include "base.h"

// Isuzu Static: the remote of an Isuzu FTS truck - a fixed 64-bit frame
// (sent back to back with a trailing "11" every 66 bits, no real gap
// between copies) plus a 4-bit button. Decode + a simple replay encoder
// (the code never changes - there's nothing to advance). See
// isuzu_static.c for the source.
#define SUBGHZ_PROTOCOL_ISUZU_STATIC_NAME "Isuzu Static"

typedef struct SubGhzProtocolDecoderIsuzuStatic SubGhzProtocolDecoderIsuzuStatic;
typedef struct SubGhzProtocolEncoderIsuzuStatic SubGhzProtocolEncoderIsuzuStatic;

extern const SubGhzProtocolDecoder subghz_protocol_isuzu_static_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_isuzu_static_encoder;
extern const SubGhzProtocol subghz_protocol_isuzu_static;

void* subghz_protocol_decoder_isuzu_static_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_isuzu_static_free(void* context);
void subghz_protocol_decoder_isuzu_static_reset(void* context);
void subghz_protocol_decoder_isuzu_static_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_isuzu_static_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_isuzu_static_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_isuzu_static_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_isuzu_static_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_isuzu_static_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_isuzu_static_free(void* context);
void subghz_protocol_encoder_isuzu_static_stop(void* context);
LevelDuration subghz_protocol_encoder_isuzu_static_yield(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_isuzu_static_deserialize(void* context, FlipperFormat* flipper_format);
