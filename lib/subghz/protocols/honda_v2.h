#pragma once

#include "base.h"

// Honda V2: this is the real identity of what this tree's own land_rover_v0.c decodes/encodes
// (81-bit frame, Lock/Unlock signature bytes 0xC20363/0xA285E3) - KAT's own research names it
// "Honda V2", not "Land Rover V0". Registered here under its correct name; land_rover_v0 is
// disabled in protocol_items.c in favor of this one. Decode + encode.
#define SUBGHZ_PROTOCOL_HONDA_V2_NAME "Honda V2"

extern const SubGhzProtocolDecoder subghz_protocol_honda_v2_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_honda_v2_encoder;
extern const SubGhzProtocol subghz_protocol_honda_v2;

void* subghz_protocol_decoder_honda_v2_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_honda_v2_free(void* context);
void subghz_protocol_decoder_honda_v2_reset(void* context);
void subghz_protocol_decoder_honda_v2_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_honda_v2_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v2_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_honda_v2_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_honda_v2_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_honda_v2_free(void* context);
void subghz_protocol_encoder_honda_v2_stop(void* context);
LevelDuration subghz_protocol_encoder_honda_v2_yield(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_honda_v2_deserialize(
    void* context,
    FlipperFormat* flipper_format);
