#pragma once

#include "base.h"

// Toyota Lexus ("Estima"): the real Toyota/Lexus rolling-code remote protocol - distinct from
// this tree's own toyota.c ("Toyota", Flipper-ARF's OLD retired dual-variant decoder, decode
// only, no real counter advance) and toyota_v0.c. KAT's own research names this "Toyota", after
// doctor-firmware's "Toyota Lexus" - registered here under that name so it doesn't collide with
// our existing "Toyota". Decode + encode: a real 7-bit counter (counting DOWN, mod 128) selects
// one of 8 XOR mask rows over a 12-byte payload; the encoder brute-forces the captured frame's
// counter (128 values x 2 "halves") to recover the plaintext, then re-encodes at counter-1. See
// toyota_lexus.c for the full writeup.
#define SUBGHZ_PROTOCOL_TOYOTA_LEXUS_NAME "Toyota Lexus"

typedef struct SubGhzProtocolDecoderToyotaLexus SubGhzProtocolDecoderToyotaLexus;
typedef struct SubGhzProtocolEncoderToyotaLexus SubGhzProtocolEncoderToyotaLexus;

extern const SubGhzProtocolDecoder subghz_protocol_toyota_lexus_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_toyota_lexus_encoder;
extern const SubGhzProtocol subghz_protocol_toyota_lexus;

void* subghz_protocol_decoder_toyota_lexus_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_toyota_lexus_reset(void* context);
void subghz_protocol_decoder_toyota_lexus_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_toyota_lexus_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_toyota_lexus_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_toyota_lexus_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_toyota_lexus_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_toyota_lexus_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus subghz_protocol_encoder_toyota_lexus_deserialize(
    void* context,
    FlipperFormat* flipper_format);
