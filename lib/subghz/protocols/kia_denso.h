#pragma once

#include "base.h"

// Kia Denso - DENSO Korea's smart keys for Kia (EV6, K5, newer Forte,
// Niro, 2019+): 112-bit Manchester frame, sent 5x back to back with no
// gap. Decode only - no button, no counter, cipher unknown. See
// kia_denso.c for the source.
#define SUBGHZ_PROTOCOL_KIA_DENSO_NAME "Kia Denso"

typedef struct SubGhzProtocolDecoderKiaDenso SubGhzProtocolDecoderKiaDenso;

extern const SubGhzProtocolDecoder subghz_protocol_kia_denso_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_kia_denso_encoder;
extern const SubGhzProtocol subghz_protocol_kia_denso;

void* subghz_protocol_decoder_kia_denso_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_denso_free(void* context);
void subghz_protocol_decoder_kia_denso_reset(void* context);
void subghz_protocol_decoder_kia_denso_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_kia_denso_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_kia_denso_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_denso_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_denso_get_string(void* context, FuriString* output);
