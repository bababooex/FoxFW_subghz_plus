#pragma once

#include "base.h"

// Volvo S40 (1999) remote - 64-bit frame (32-bit ID, 8-bit press counter,
// 2 unknown bytes), same air format/framing rule as our chrysler.c family
// but with a 64-bit (not 80-bit) frame. Decode only, no button field (only
// one button was ever recorded by KAT, so which byte carries it isn't
// known). See volvo_s40.c for the source.
#define SUBGHZ_PROTOCOL_VOLVO_S40_NAME "Volvo S40"

typedef struct SubGhzProtocolDecoderVolvoS40 SubGhzProtocolDecoderVolvoS40;

extern const SubGhzProtocolDecoder subghz_protocol_volvo_s40_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_volvo_s40_encoder;
extern const SubGhzProtocol subghz_protocol_volvo_s40;

void* subghz_protocol_decoder_volvo_s40_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_volvo_s40_free(void* context);
void subghz_protocol_decoder_volvo_s40_reset(void* context);
void subghz_protocol_decoder_volvo_s40_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_volvo_s40_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_volvo_s40_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_volvo_s40_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_volvo_s40_get_string(void* context, FuriString* output);
