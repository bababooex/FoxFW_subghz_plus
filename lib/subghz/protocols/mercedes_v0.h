#pragma once

#include "base.h"

// Mercedes V0: a different Mercedes remote scheme from mercedes.c (old, AM/FM decode-only
// KeeLoq-layout) and mercedes_v1.c - 112-bit Manchester-coded frame, static (non-rolling)
// payload with a 4-bit command nibble (Lock/Unlock/Follow-up), no counter. Decode + encode.
#define SUBGHZ_PROTOCOL_MERCEDES_V0_NAME "Mercedes V0"

extern const SubGhzProtocol subghz_protocol_mercedes_v0;
