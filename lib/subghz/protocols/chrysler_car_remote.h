#pragma once

#include "base.h"

// Chrysler car remote, 315 MHz - 48-bit fixed code (32-bit ID + button).
// Decode only. Not the same protocol as ProtoPirate's/our "Chrysler V0" -
// see chrysler_car_remote.c.
#define SUBGHZ_PROTOCOL_CHRYSLER_CAR_REMOTE_NAME "Chrysler-CarRemote"

typedef struct SubGhzProtocolDecoderChryslerCarRemote SubGhzProtocolDecoderChryslerCarRemote;

extern const SubGhzProtocolDecoder subghz_protocol_chrysler_car_remote_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_chrysler_car_remote_encoder;
extern const SubGhzProtocol subghz_protocol_chrysler_car_remote;

void* subghz_protocol_decoder_chrysler_car_remote_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_chrysler_car_remote_free(void* context);
void subghz_protocol_decoder_chrysler_car_remote_reset(void* context);
void subghz_protocol_decoder_chrysler_car_remote_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_chrysler_car_remote_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_chrysler_car_remote_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_chrysler_car_remote_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_chrysler_car_remote_get_string(void* context, FuriString* output);
