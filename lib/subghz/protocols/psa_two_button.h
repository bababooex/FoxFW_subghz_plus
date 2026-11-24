#pragma once

#include "base.h"

// PSA 2-Button - a two-button Valeo flip key for Citroen/Peugeot: 76-bit
// Manchester frame (a fixed byte, a button nibble, a 64-bit code of
// unknown cipher). Decode only. Not the same protocol as our psa.c/psa2.c
// (80-bit TEA/XOR frame). See psa_two_button.c for the source.
#define SUBGHZ_PROTOCOL_PSA_TWO_BUTTON_NAME "PSA 2-Button"

typedef struct SubGhzProtocolDecoderPsaTwoButton SubGhzProtocolDecoderPsaTwoButton;

extern const SubGhzProtocolDecoder subghz_protocol_psa_two_button_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_psa_two_button_encoder;
extern const SubGhzProtocol subghz_protocol_psa_two_button;

void* subghz_protocol_decoder_psa_two_button_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_psa_two_button_free(void* context);
void subghz_protocol_decoder_psa_two_button_reset(void* context);
void subghz_protocol_decoder_psa_two_button_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_psa_two_button_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_psa_two_button_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_psa_two_button_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_psa_two_button_get_string(void* context, FuriString* output);
