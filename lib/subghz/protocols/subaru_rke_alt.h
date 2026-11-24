#pragma once

#include "base.h"

// EXPERIMENTAL / UNVALIDATED - see subaru_rke_alt.c for details. Distinct from
// the already-registered "SUBARU" protocol (subaru.c) - different, untested frame.
#define SUBGHZ_PROTOCOL_SUBARU_RKE_ALT_NAME "Subaru RKE Alt (unverified)"

typedef struct SubGhzProtocolDecoderSubaruRkeAlt SubGhzProtocolDecoderSubaruRkeAlt;

extern const SubGhzProtocolDecoder subghz_protocol_subaru_rke_alt_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_subaru_rke_alt_encoder;
extern const SubGhzProtocol subghz_protocol_subaru_rke_alt;

void* subghz_protocol_decoder_subaru_rke_alt_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_subaru_rke_alt_free(void* context);
void subghz_protocol_decoder_subaru_rke_alt_reset(void* context);
void subghz_protocol_decoder_subaru_rke_alt_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_subaru_rke_alt_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_subaru_rke_alt_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_subaru_rke_alt_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_subaru_rke_alt_get_string(void* context, FuriString* output);
