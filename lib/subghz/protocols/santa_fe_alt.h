#pragma once

#include "base.h"

// EXPERIMENTAL / UNVALIDATED - see santa_fe_alt.c for details. Distinct from
// the already-registered "SantaFe 13-16" protocol (santa_fe.c).
#define SUBGHZ_PROTOCOL_SANTA_FE_ALT_NAME "SantaFe Alt (unverified)"

typedef struct SubGhzProtocolDecoderSantaFeAlt SubGhzProtocolDecoderSantaFeAlt;

extern const SubGhzProtocolDecoder subghz_protocol_santa_fe_alt_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_santa_fe_alt_encoder;
extern const SubGhzProtocol subghz_protocol_santa_fe_alt;

void* subghz_protocol_decoder_santa_fe_alt_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_santa_fe_alt_free(void* context);
void subghz_protocol_decoder_santa_fe_alt_reset(void* context);
void subghz_protocol_decoder_santa_fe_alt_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_santa_fe_alt_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_santa_fe_alt_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_santa_fe_alt_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_santa_fe_alt_get_string(void* context, FuriString* output);
