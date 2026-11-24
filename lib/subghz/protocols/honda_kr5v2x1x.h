#pragma once

#include "base.h"

// Honda Keyfob KR5V2X/KR5V1X - an alternate decoding of the same physical
// remotes as our continental_rke.c (rtl_433 added this one as a second,
// disagreeing bit layout/checksum for the same fobs). 120-bit frame behind
// a fixed 24-bit manufacturer code, device ID + counter + rolling code in
// the clear, CRC-8 checked. Decode only, cipher unknown for the rolling
// part. No real capture validates this anywhere (not even in rtl_433
// upstream) - see honda_kr5v2x1x.c.
#define SUBGHZ_PROTOCOL_HONDA_KR5V2X1X_NAME "Honda-KR5V2X1X"

typedef struct SubGhzProtocolDecoderHondaKr5v2x1x SubGhzProtocolDecoderHondaKr5v2x1x;

extern const SubGhzProtocolDecoder subghz_protocol_honda_kr5v2x1x_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_honda_kr5v2x1x_encoder;
extern const SubGhzProtocol subghz_protocol_honda_kr5v2x1x;

void* subghz_protocol_decoder_honda_kr5v2x1x_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_honda_kr5v2x1x_free(void* context);
void subghz_protocol_decoder_honda_kr5v2x1x_reset(void* context);
void subghz_protocol_decoder_honda_kr5v2x1x_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_honda_kr5v2x1x_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_honda_kr5v2x1x_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_honda_kr5v2x1x_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_honda_kr5v2x1x_get_string(void* context, FuriString* output);
