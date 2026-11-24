#pragma once

#include "base.h"

// Renault 1-Button - an old one-button Renault remote (board dated 1999):
// 72-bit Manchester frame (24-bit ID, 36-bit rolling part, 8-bit XOR
// check). Decode only - the rolling part can't be continued or predicted,
// no known cipher. See renault_one_button.c for the source. Not any of
// our other renault_*.c protocols.
#define SUBGHZ_PROTOCOL_RENAULT_ONE_BUTTON_NAME "Renault 1-Button"

typedef struct SubGhzProtocolDecoderRenaultOneButton SubGhzProtocolDecoderRenaultOneButton;

extern const SubGhzProtocolDecoder subghz_protocol_renault_one_button_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_renault_one_button_encoder;
extern const SubGhzProtocol subghz_protocol_renault_one_button;

void* subghz_protocol_decoder_renault_one_button_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_renault_one_button_free(void* context);
void subghz_protocol_decoder_renault_one_button_reset(void* context);
void subghz_protocol_decoder_renault_one_button_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_renault_one_button_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_renault_one_button_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_renault_one_button_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_renault_one_button_get_string(void* context, FuriString* output);
