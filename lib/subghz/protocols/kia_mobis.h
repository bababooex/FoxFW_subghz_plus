#pragma once

#include "base.h"

// Kia Mobis - Hyundai Mobis smart keys for Kia/Hyundai (2017-2019 boards):
// 72-bit Manchester frame behind a fixed 16-bit word, with the press
// counter and button in the clear. Decode only - no fob serial, no known
// cipher for the remaining bits. See kia_mobis.c for the source.
#define SUBGHZ_PROTOCOL_KIA_MOBIS_NAME "Kia Mobis"

typedef struct SubGhzProtocolDecoderKiaMobis SubGhzProtocolDecoderKiaMobis;

extern const SubGhzProtocolDecoder subghz_protocol_kia_mobis_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_kia_mobis_encoder;
extern const SubGhzProtocol subghz_protocol_kia_mobis;

void* subghz_protocol_decoder_kia_mobis_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_mobis_free(void* context);
void subghz_protocol_decoder_kia_mobis_reset(void* context);
void subghz_protocol_decoder_kia_mobis_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_kia_mobis_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_kia_mobis_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_mobis_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_mobis_get_string(void* context, FuriString* output);
