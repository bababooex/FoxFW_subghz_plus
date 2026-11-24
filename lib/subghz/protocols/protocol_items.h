#pragma once
#include "../registry.h"
#include "../subghz_protocol_registry.h"

#include "princeton.h"
#include "cardin_s508.h"
#include "prastel.h"
#include "hormann_bisecur.h"
#include "keeloq.h"
#include "nice_flo.h"
#include "came.h"
#include "faac_slh.h"
#include "nice_flor_s.h"
#include "came_twee.h"
#include "came_atomo.h"
#include "nero_sketch.h"
#include "ido.h"
#include "hormann.h"
#include "nero_radio.h"
#include "somfy_telis.h"
#include "somfy_keytis.h"
#include "gate_tx.h"
#include "raw.h"
#include "linear.h"
#include "linear_delta3.h"
#include "secplus_v2.h"
#include "secplus_v1.h"
#include "megacode.h"
#include "holtek.h"
#include "chamberlain_code.h"
#include "power_smart.h"
#include "marantec.h"
#include "bett.h"
#include "doitrand.h"
#include "phoenix_v2.h"
#include "honeywell_wdb.h"
#include "magellan.h"
#include "intertechno_v3.h"
#include "clemsa.h"
#include "ansonic.h"
#include "smc5326.h"
#include "holtek_ht12x.h"
#include "dooya.h"
#include "alutech_at_4n.h"
#include "kinggates_stylo_4k.h"
#include "bin_raw.h"
#include "mastercode.h"
#include "honeywell.h"
#include "legrand.h"
#include "dickert_mahs.h"
#include "gangqi.h"
#include "marantec24.h"
#include "hollarm.h"
#include "hay21.h"
#include "revers_rb2.h"
#include "feron.h"
#include "roger.h"
#include "elplast.h"
#include "telcoma_edge.h"
#include "treadmill37.h"
#include "beninca_arc.h"
#include "keyfinder.h"
#include "jarolift.h"
#include "vag.h"
#include "porsche_cayenne.h"
#include "ford_v0.h"
#include "psa.h"
#include "fiat_spa.h"
#include "fiat_marelli.h"
#include "bmw_cas4.h"
#include "subaru.h"
#include "kia_generic.h"
#include "kia_v0.h"
#include "kia_v1.h"
#include "kia_v2.h"
#include "kia_v3_v4.h"
#include "kia_v5.h"
#include "kia_v6.h"
#include "suzuki.h"
#include "mitsubishi_v0.h"
#include "mitsubishi_v0a.h"
#include "mazda_siemens.h"
#include "star_line.h"
#include "scher_khan.h"
#include "sheriff_cfm.h"
#include "chrysler.h"
#include "honda_static.h"
#include "mazda_v0.h"
#include "kia_v7.h"
#include "ford_v1.h"
#include "ford_v2.h"
#include "ford_v3.h"
#include "honda_v1.h"
#include "fiat_v0.h"
#include "fiat_v1.h"
#include "fiat_v2.h"
#include "renault_v0.h"
#include "renault_v1.h"
#include "gm.h"
#include "land_rover_v0.h"
// Honda V2 (2026-11-22): KAT's own research says this is the real identity of what
// land_rover_v0.c decodes - land_rover_v0 is disabled below in favor of this one.
#include "honda_v2.h"
#include "toyota.h"
#include "x10.h"
#include "ducato_v0.h"
// --- FoxFW2.0 2026-10 protocol port batch (decode-only; KeeLoq-RX arke ports) ---
#include "jeep_v0.h"
#include "ldv_v0.h"
#include "renault_v2.h"
#include "renault_v3.h"
#include "renault_v4.h"
#include "audi_v2.h"
#include "audi_v3.h"
#include "nissan_v0.h"
#include "vag_polo.h"
#include "chevrolet_v0.h"
#include "mazda_v1.h"
#include "mazda_v2.h"
#include "bmw_v2.h"
#include "bmw_v3.h"
#include "lexus_v0.h"
#include "mitsubishi_v1.h"
#include "honda_v4.h"
#include "honda_v5.h"
#include "honda_v6.h"
#include "honda_v7.h"
#include "honda_v8.h"
#include "honda_v9.h"
#include "honda_v10.h"
// Dormant decoders ported from ARF (2026-10-03) — headers declared so the
// commented-out registry entries in protocol_items.c are ready to enable.
#include "mercedes.h"
#include "mazda_infinity.h"
#include "audi.h"
#include "hundai.h"
// === FoxFW2.0 v2.1 2026-10-21: formerly-externalized autos brought LIVE ===
// Moved from applications/main/subghz/protocols_ext/ into lib; the three
// subghz_ext_auto_g1/g2/g3 .fal groups are DISABLED in application.fam.
// To re-externalize a brand, see UNEXTERNALIZE_PROCEDURE doc.
#include "fiat_v2_v3_v5.h"
#include "toyota_v0.h"
#include "honda_v2_v3.h"
#include "hyundai_kia_rio.h"
#include "santa_fe.h"
#include "peugot_v0.h"
#include "audi_v1.h"
#include "gm_rke.h"
#include "tesla_charge.h"
#include "opel_v0.h"
#include "continental_rke.h"
#include "nidec_rke.h"
#include "siemens_vdo.h"
#include "hitag2.h"
#include "alps_rke.h"
#include "mic6sc2.h"
#include "porsche_touareg.h"
#include "code_alarm.h"
#include "astrostart.h"
#include "audiovox.h"
#include "compustar.h"

// SubRATT-only protocols promoted into main registry (FoxFW2.0 v2.1 2026-10-21)
#include "allstar_firefly.h"

// Remaining in-tree protocols brought LIVE (FoxFW2.0 v2.1 2026-10-22): detector internal,
// decode_*.fal + enc_*.fal on SD. mercedes_fob stays shelved (#if 0 placeholder timings).
#include "ditec_gol4.h"
#include "nord_ice.h"
#include "psa2.h"

// Ported from doctor-firmware (FoxFW2.0 v2.1 2026-10-22): detector internal, decode/encode on SD.
#include "urmet.h"
#include "keyfinder2.h"
#include "doorbell32.h"
#include "opel_v1.h"
#include "bmw_cas3.h"
#include "bmw_cas4.h"
#include "datsun_lada.h"

// Real scheme, not a guess - see vag_v1.c's header comment (24-bit LFSR
// obfuscation, Garcia et al., USENIX Security 2016; ported from the KAT
// project's independent re-derivation). Decode + encode.
#include "vag_v1.h"

// Ported from RogueMaster's lib/subghz/protocols/proteco.c (another Flipper
// firmware fork), cross-checked against KAT's own re-derivation. Real
// captures behind it (RogueMaster's unit-test .sub files). Decode + encode.
#include "proteco.h"

// Ported from RogueMaster (another Flipper firmware fork; this tree didn't
// have them), cross-checked against KAT's own re-derivation. Decode + encode.
#include "faac_tm.h"
#include "freezer.h"

// Ported from rtl_433 (REFERENCES/rtl_433, GPL-2.0), cross-checked against
// KAT's own re-derivation. Decode only, as rtl_433 itself (receive only) -
// a captured signal can still be replayed via Flipper's raw-replay.
#include "cardin_s466.h"
#include "chrysler_car_remote.h"
#include "siemens_5wy72xx.h"

// Ported from rtl_433, decode only, and only the chip's single most common
// EEPROM-selected timing/mode (by deliberate scope choice - see each file's
// header comment for the other timings this does NOT cover).
#include "microchip_hcs361.h"
#include "microchip_hcs362.h"

// KAT's own decoders, each worked out from real recordings (not ported
// from any other firmware or rtl_433); decode only except Volvo S40 (no
// button field at all), Opel Rolling and Suzuki TS002 - those two also
// have an encoder, since their "rolling" part is an obfuscation KAT fully
// reversed (not a real cipher) - see each file's own header.
#include "volvo_s40.h"
#include "gm_rfa.h"
#include "kia_denso.h"
#include "kia_forte.h"
#include "kia_mobis.h"
#include "psa_two_button.h"
#include "renault_one_button.h"
#include "opel_rolling.h"
#include "suzuki_ts002.h"
#include "lexus_ux.h"
#include "isuzu_static.h"
#include "isuzu_rolling.h"

// Ported from rtl_433, decode only. UNVALIDATED: no real capture exists
// anywhere (not even in rtl_433's own test suite) - see the file header.
#include "honda_kr5v2x1x.h"

// EXPERIMENTAL / UNVALIDATED (2026-11-16): ported on request from the untested
// scaffolding in auto_rke_protocols.c, decode-only, no cited source or real
// capture behind the timing/checksum numbers. See each .c file's header
// comment before trusting a "valid" decode from any of these four.
#include "hkr.h"
#include "mazda_v3.h"
#include "subaru_rke_alt.h"
#include "santa_fe_alt.h"

// Adapted from a reference C port onto this tree's actual generic/custom-button
// API (2026-11-21) - see each file's header for what changed. Decode + encode;
// the rolling-code "hop" payload is cipher-unknown and carried forward
// unchanged, like this session's other such protocols.
#include "bmw.h"
#include "mercedes_v1.h"

// Real Toyota/Lexus "Estima" protocol (2026-11-22), ported from a reference C
// implementation onto this tree's generic/custom-button API. Registered as
// "Toyota Lexus" to avoid colliding with this tree's existing toyota.c ("Toyota",
// ARF's old retired dual-variant decoder - see toyota_lexus.h's header comment).
// Decode + encode, with real brute-force counter recovery (not cipher-unknown).
#include "toyota_lexus.h"

// Mercedes V0 (2026-11-22): a static (non-rolling), 112-bit Manchester-coded Mercedes remote
// scheme, distinct from mercedes.c (old AM/FM KeeLoq-layout decode) and mercedes_v1.c (rolling).
// Adapted from a reference C implementation onto this tree's API. Decode + encode.
#include "mercedes_v0.h"
