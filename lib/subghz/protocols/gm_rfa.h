#pragma once

#include "base.h"

// GM RFA - the Omron "Remote Function Actuator" remotes used on GM cars
// (Cadillac/GMC/Chevrolet/Buick, ~2004-2017): 84-bit Manchester frame (4-bit
// head, 16-bit fob ID, 64-bit rolling code of unknown cipher). Decode only
// - no counter, no button, and the code can't be decrypted. See gm_rfa.c
// for the source.
#define SUBGHZ_PROTOCOL_GM_RFA_NAME "GM RFA"

typedef struct SubGhzProtocolDecoderGmRfa SubGhzProtocolDecoderGmRfa;

extern const SubGhzProtocolDecoder subghz_protocol_gm_rfa_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_gm_rfa_encoder;
extern const SubGhzProtocol subghz_protocol_gm_rfa;

void* subghz_protocol_decoder_gm_rfa_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_gm_rfa_free(void* context);
void subghz_protocol_decoder_gm_rfa_reset(void* context);
void subghz_protocol_decoder_gm_rfa_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_gm_rfa_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_gm_rfa_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_gm_rfa_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_gm_rfa_get_string(void* context, FuriString* output);
