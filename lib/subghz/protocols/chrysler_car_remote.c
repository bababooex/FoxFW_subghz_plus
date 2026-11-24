#include "chrysler_car_remote.h"
#include "../blocks/decoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <stdlib.h>

/*
 * Chrysler car remote, 315 MHz - 48-bit fixed code (32-bit ID, 4-bit
 * button), DECODE ONLY. Ported from rtl_433's src/devices/
 * chrysler_car_remote.c (r_device "Chrysler Car Remote"; part numbers
 * 56008761, 56008762 (FCC ID GQ43VT7T), 04686366, 56021903AA), cross-
 * checked against the KAT project's port (github.com/KaraZajac/KAT,
 * src/protocols/chrysler_car_remote.rs). This is a DIFFERENT air format
 * from our "Chrysler V0" / ProtoPirate's (300/600us pulses, 3.4ms gaps,
 * 80 bits) - that decoder does not read these remotes, and this one won't
 * read Chrysler V0's.
 *
 * Air format (AM/OOK): a bit's value comes purely from its own HIGH (mark)
 * pulse width - short (~350us) = bit 1, long (~700us) = bit 0; a ~7350us
 * mark is a sync (not a bit). The LOW (space) that follows only decides
 * whether the current run of bits continues (<=4200us) or ends here
 * (>4200us) - a sync pulse also always ends/restarts the run. A press
 * sends: a sync, a ~25-bit preamble, then the 48-bit packet TWICE (the
 * first copy picks up one extra trailing bit, dropped before decoding),
 * each run separated by one of these gaps. Each valid 48-bit packet found
 * is reported on its own (so a press typically reports twice).
 *
 * Frame (48 bits, each byte bit-inverted and bit-reversed from the wire):
 * a 32-bit ID, a 4-bit button nibble (bit 0x1 Unlock, 0x2 Lock, 0x4 Panic;
 * more than one may be held at once), one unused bit, a "multi-press" flag
 * that must be set exactly when more than one button bit is, two more
 * unused bits, and an 8-bit checksum (the sum of the first 5 bytes). All-
 * zero and all-0xFF frames are rejected as implausible (noise), not just
 * the checksum.
 *
 * No encoder, matching rtl_433 (receive only) - a captured signal can
 * still be replayed via Flipper's generic raw-replay.
 */

#define CCR_BITS 48u
#define CCR_SHORT_NOM 350u
#define CCR_LONG_NOM 700u
#define CCR_SYNC_NOM 7350u
#define CCR_TOL 100u
#define CCR_TOO_SHORT_MAX 250u
#define CCR_ROW_GAP_LIMIT 4200u

#define CCR_IN_TOL(x, ref) (DURATION_DIFF((x), (ref)) <= CCR_TOL)

struct SubGhzProtocolDecoderChryslerCarRemote {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
};

static const char* const chrysler_car_remote_buttons[3] = {"Unlock", "Lock", "Panic"};

const SubGhzProtocolDecoder subghz_protocol_chrysler_car_remote_decoder = {
    .alloc = subghz_protocol_decoder_chrysler_car_remote_alloc,
    .free = subghz_protocol_decoder_chrysler_car_remote_free,
    .reset = subghz_protocol_decoder_chrysler_car_remote_reset,
    .feed = subghz_protocol_decoder_chrysler_car_remote_feed,
    .get_hash_data = subghz_protocol_decoder_chrysler_car_remote_get_hash_data,
    .serialize = subghz_protocol_decoder_chrysler_car_remote_serialize,
    .deserialize = subghz_protocol_decoder_chrysler_car_remote_deserialize,
    .get_string = subghz_protocol_decoder_chrysler_car_remote_get_string,
};

/* Decode-only, as rtl_433. */
const SubGhzProtocolEncoder subghz_protocol_chrysler_car_remote_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_chrysler_car_remote = {
    .name = SUBGHZ_PROTOCOL_CHRYSLER_CAR_REMOTE_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_chrysler_car_remote_decoder,
    .encoder = &subghz_protocol_chrysler_car_remote_encoder,
};

/* One byte bit-inverted then bit-reversed (the C's bitbuffer_invert +
 * reflect_bytes), matching rtl_433's own wire convention for this remote. */
static uint8_t chrysler_car_remote_unwire_byte(uint8_t w) {
    uint8_t v = (uint8_t)~w;
    uint8_t out = 0;
    for(int i = 0; i < 8; i++) {
        out = (uint8_t)((out << 1) | (v & 1));
        v >>= 1;
    }
    return out;
}

/* Validates and extracts fields from the 48 raw wire bits of one candidate
 * packet (before the unwire transform, right-aligned in a 64-bit value). */
static bool chrysler_car_remote_fields(SubGhzBlockGeneric* generic, uint64_t wire48) {
    uint8_t w[6];
    for(int i = 0; i < 6; i++) w[i] = (uint8_t)(wire48 >> (8 * (5 - i)));
    uint8_t b[6];
    for(int i = 0; i < 6; i++) b[i] = chrysler_car_remote_unwire_byte(w[i]);

    uint32_t sum = (uint32_t)b[0] + b[1] + b[2] + b[3] + b[4];
    if((sum & 0xFFu) != b[5]) return false; /* checksum */
    if(sum == 0 || sum == 0xFFu * 5u) return false; /* all-zero / all-one noise */

    uint8_t button = (uint8_t)(b[4] >> 4);
    bool multi_press = (b[4] & 0x04u) != 0;
    int matches = __builtin_popcount(button & 0x7u);
    if(matches == 0) return false;
    if(matches > 1 && !multi_press) return false;
    if(matches == 1 && multi_press) return false;

    generic->data = wire48;
    generic->data_count_bit = CCR_BITS;
    generic->serial = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) |
                       (uint32_t)b[3];
    generic->btn = button;
    return true;
}

void* subghz_protocol_decoder_chrysler_car_remote_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderChryslerCarRemote* instance = calloc(1, sizeof(*instance));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_chrysler_car_remote;
    instance->generic.protocol_name = SUBGHZ_PROTOCOL_CHRYSLER_CAR_REMOTE_NAME;
    return instance;
}

void subghz_protocol_decoder_chrysler_car_remote_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_chrysler_car_remote_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderChryslerCarRemote* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

/* Tries to decode whatever is accumulated as a trailing-48-bit window (the
 * last 48 bits received, or - when more than 48 arrived - the 48 bits
 * ending one bit before the end, since a long run's very last bit is a
 * spurious extra one the real decoder also drops), then clears the run. */
static void chrysler_car_remote_finalize_row(SubGhzProtocolDecoderChryslerCarRemote* instance) {
    uint8_t count = instance->decoder.decode_count_bit;
    if(count >= CCR_BITS) {
        uint64_t window = (count == CCR_BITS) ? instance->decoder.decode_data :
                                                 (instance->decoder.decode_data >> 1);
        window &= 0xFFFFFFFFFFFFULL;
        if(chrysler_car_remote_fields(&instance->generic, window)) {
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
    }
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

void subghz_protocol_decoder_chrysler_car_remote_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderChryslerCarRemote* instance = context;

    if(level) {
        if(CCR_IN_TOL(duration, CCR_SYNC_NOM)) {
            /* A sync mark - not a bit; ends whatever run was in progress. */
            chrysler_car_remote_finalize_row(instance);
            return;
        }
        if(duration <= CCR_TOO_SHORT_MAX) {
            /* A too-short glitch: dropped entirely, the run is unaffected. */
            return;
        }
        if(CCR_IN_TOL(duration, CCR_SHORT_NOM)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 1);
        } else if(CCR_IN_TOL(duration, CCR_LONG_NOM)) {
            subghz_protocol_blocks_add_bit(&instance->decoder, 0);
        } else {
            /* Ambiguous mark width: ends the run without contributing a bit. */
            chrysler_car_remote_finalize_row(instance);
        }
    } else {
        if(duration > CCR_ROW_GAP_LIMIT) {
            chrysler_car_remote_finalize_row(instance);
        }
        /* A short gap just continues the run - no bit value of its own. */
    }
}

uint8_t subghz_protocol_decoder_chrysler_car_remote_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderChryslerCarRemote* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_chrysler_car_remote_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderChryslerCarRemote* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_chrysler_car_remote_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderChryslerCarRemote* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, CCR_BITS);
    if(status != SubGhzProtocolStatusOk) return status;
    if(!chrysler_car_remote_fields(&instance->generic, instance->generic.data))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_chrysler_car_remote_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderChryslerCarRemote* instance = context;
    FuriString* btn_str = furi_string_alloc();
    uint8_t btn = instance->generic.btn;
    furi_string_cat_str(btn_str, "[");
    for(int i = 0; i < 3; i++) {
        if(btn & (1u << i)) {
            if(furi_string_size(btn_str) > 1) furi_string_cat_str(btn_str, "; ");
            furi_string_cat_str(btn_str, chrysler_car_remote_buttons[i]);
        }
    }
    furi_string_cat_str(btn_str, "]");
    furi_string_cat_printf(
        output,
        "%s 48bit\r\n"
        "Key:%012llX\r\n"
        "Id:%08lX Btn:%02X %s\r\n"
        "Fixed code - no counter\r\n",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        furi_string_get_cstr(btn_str));
    furi_string_free(btn_str);
}
