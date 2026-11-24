#pragma once

#include "base.h"

// EXPERIMENTAL / UNVALIDATED - see hkr.c for details.
#define SUBGHZ_PROTOCOL_HKR_NAME "HKR (unverified)"

typedef struct SubGhzProtocolDecoderHkr SubGhzProtocolDecoderHkr;

extern const SubGhzProtocolDecoder subghz_protocol_hkr_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_hkr_encoder;
extern const SubGhzProtocol subghz_protocol_hkr;

void* subghz_protocol_decoder_hkr_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_hkr_free(void* context);
void subghz_protocol_decoder_hkr_reset(void* context);
void subghz_protocol_decoder_hkr_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_hkr_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_hkr_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_hkr_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_hkr_get_string(void* context, FuriString* output);
