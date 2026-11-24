#pragma once

#include "base.h"

// Kia Forte - a 315 MHz Kia remote, 88-bit Manchester frame (11 bytes),
// sent 4x per press (not back to back - real gaps between copies).
// Decode only - cipher unknown. See kia_forte.c for the source.
#define SUBGHZ_PROTOCOL_KIA_FORTE_NAME "Kia Forte"

typedef struct SubGhzProtocolDecoderKiaForte SubGhzProtocolDecoderKiaForte;

extern const SubGhzProtocolDecoder subghz_protocol_kia_forte_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_kia_forte_encoder;
extern const SubGhzProtocol subghz_protocol_kia_forte;

void* subghz_protocol_decoder_kia_forte_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_forte_free(void* context);
void subghz_protocol_decoder_kia_forte_reset(void* context);
void subghz_protocol_decoder_kia_forte_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_kia_forte_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_kia_forte_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_forte_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_forte_get_string(void* context, FuriString* output);
