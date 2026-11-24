#include "custom_btn_i.h"
#include <furi.h>
#include <string.h>

typedef struct {
    const char* proto;
    const char* up;
    const char* down;
    const char* left;
    const char* right;
} SubGhzBtnLabelEntry;

static const SubGhzBtnLabelEntry label_table[] = {

    { "KIA/HYU V0",   "Lock",  "Unlock", "Trunk",  "Panic"  },
    { "KIA/HYU V1",   "Lock",  "Unlock", "Trunk",  "Panic"  },
    { "KIA/HYU V2",   "Lock",  "Unlock", "Trunk",  "Panic"  },
    { "KIA/HYU V3/V4","Lock",  "Unlock", "Trunk",  "Panic"  },
    { "KIA/HYU V5",   "Lock",  "Unlock", "Trunk",  "Horn"   },
    { "KIA/HYU V6",   "Lock",  "Unlock", "Trunk",  "Panic"  },
    { "Kia V7",       "Lock",  "Unlock", "Trunk",  "Panic"  },

    { "VAG GROUP",    "Lock",  "Unlock", "Trunk",  "Panic"  },
    { "Porsche AG",   "Lock",  "Unlock", "Trunk",  "Panic"  },
    { "VAG V1 (VW-1 Legacy)", "Lock", "Unlock", NULL, NULL },

    { "Proteco",      "A",     "B",      "C",      "4th"    },
    { "Opel Rolling", "Lock",  "Unlock", NULL,     NULL     },
    { "Suzuki TS002", "Lock",  "Unlock", NULL,     NULL     },
    { "BMW",          "Unlock","Lock",   NULL,     NULL     },
    { "Mercedes V1",  "Unlock","Lock",   NULL,     NULL     },
    { "Mercedes V0",  "Unlock","Lock",   NULL,     NULL     },
    { "Toyota Lexus", "Cmd 1", "Cmd 2",  "Cmd 3",  NULL     },
    { "Honda V2",     "Lock",  "Unlock", NULL,     NULL     },

    { "PSA GROUP",    "Lock",  "Unlock", "Trunk",  "Trunk"  },
    { "PSA OLD",      "Lock",  "Unlock", "Trunk",  "Trunk"  },

    { "FORD V0",      "Lock",  "Unlock", "Trunk",  NULL     },
    { "Ford V1",      "Unlock","Panic",  "Lock",   "Trunk"  },

    { "MARELLI",      "Lock",  "Unlock", "Trunk",  NULL     },
    { "FIAT SPA",     "Lock",  "Unlock", "Trunk",  "Panic"  },
    { "Fiat V1",      "Unlock","Lock",   "Trunk",   "Close"  },

    { "Chrysler",     "Lock",  "Unlock", NULL,     NULL     },

    { "SUBARU",       "Lock",  "Unlock", "Trunk",  "Panic"  },
    { "SUZUKI",       "Panic", "Trunk",  "Lock",   "Unlock" },
    { "Mazda V0",     "Lock",  "Trunk", "Unlock",  "Panic"  },
    { "MazdaSiemens", "Lock",  "Unlock", "Trunk",  "Panic"  },
    { "Mitsubishi V0","Lock",  "Unlock", NULL,     NULL     },
    { "Mitsubishi V0-a","Lock","Unlock", "Trunk",  "Panic"  },

    { "Scher-Khan",   "Lock",  "Unlock", "Trunk",  "Start"  },
    { "Star Line",    "Lock",  "Unlock", "Trunk",  "Start"  },
    { "Sheriff CFM",  "Lock",  "Unlock", "Trunk",  "Panic"  },

    { "Hitag-2",      "Lock",  "Unlock", NULL,     NULL     },
    { "Renault V0",   "Lock",  "Unlock", NULL,     NULL     },
    { "Datsun/Lada",  "Btn A", "Btn B", "Btn C",  "Btn D"  },
    { "Honda V1",     "Unlock","Lock",  "Trunk",  "Panic"  },


    { NULL, NULL, NULL, NULL, NULL }
};

/* Pure, read-only preview of what lib/subghz/protocols/keeloq.c's
 * subghz_protocol_keeloq_get_btn_code() would pick for a given direction -
 * deliberately DUPLICATED here rather than shared, so this label-only
 * helper can never touch the real encode path (keeloq.c / enc_keeloq.c):
 * worst case this hint is wrong, it can never change what gets transmitted.
 * Mirrors the plain last_btn_code=0xA default; for the handful of
 * manufacturers with a different "programming button" default (BFT,
 * Aprimatic, AN-Motors...) the real RIGHT press can differ from this hint -
 * it's a label, not a promise. */
uint8_t subghz_custom_btn_keeloq_preview_code(uint8_t original_btn_code, uint8_t direction) {
    const uint8_t last_btn_code = 0xA;
    uint8_t btn = original_btn_code;
    switch(direction) {
    case SUBGHZ_CUSTOM_BTN_UP:
        switch(original_btn_code) {
        case 0x1: btn = 0x2; break;
        case 0x2: btn = 0x1; break;
        case 0xA: btn = 0x1; break;
        case 0x4: btn = 0x1; break;
        case 0x8: btn = 0x1; break;
        case 0xF: btn = 0x1; break;
        case 0x9: btn = 0x2; break;
        case 0x6: btn = 0x2; break;
        default: btn = 0x1; break;
        }
        break;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        switch(original_btn_code) {
        case 0x1: btn = 0x4; break;
        case 0x2: btn = 0x4; break;
        case 0xA: btn = 0x4; break;
        case 0x4: btn = last_btn_code; break;
        case 0x8: btn = 0x4; break;
        case 0xF: btn = 0x4; break;
        case 0x9: btn = 0x4; break;
        case 0x6: btn = 0x4; break;
        default: btn = 0x4; break;
        }
        break;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        switch(original_btn_code) {
        case 0x1: btn = 0x8; break;
        case 0x2: btn = 0x8; break;
        case 0xA: btn = 0x8; break;
        case 0x4: btn = 0x8; break;
        case 0x8: btn = 0x2; break;
        case 0xF: btn = 0x8; break;
        case 0x9: btn = 0x6; break;
        case 0x6: btn = 0x9; break;
        default: btn = 0x8; break;
        }
        break;
    case SUBGHZ_CUSTOM_BTN_RIGHT:
        switch(original_btn_code) {
        case 0x1: btn = last_btn_code; break;
        case 0x2: btn = last_btn_code; break;
        case 0xA: btn = 0x2; break;
        case 0x4: btn = 0x2; break;
        case 0x8: btn = last_btn_code; break;
        case 0xF: btn = 0x2; break;
        case 0x9: btn = last_btn_code; break;
        case 0x6: btn = last_btn_code; break;
        default: btn = last_btn_code; break;
        }
        break;
    default:
        break;
    }
    return btn;
}

const char* subghz_custom_btn_get_label_for_proto(
    const char* proto_name, uint8_t btn_dir) {

    if(!proto_name || btn_dir == 0 || btn_dir > 4) return NULL;

    for(const SubGhzBtnLabelEntry* e = label_table; e->proto; e++) {
        if(strcmp(e->proto, proto_name) == 0) {
            switch(btn_dir) {
            case 1: return e->up;
            case 2: return e->down;
            case 3: return e->left;
            case 4: return e->right;
            }
        }
    }
    return NULL;
}
