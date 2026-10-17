/*
 * hle_hid.c - the table behind hle_hid.h. See that file for what the two numbers
 * are for.
 */
#include "hle_hid.h"

int hle_hid_to_win(int hid, int *vk, int *scan) {
    int v = 0, s = 0;

    if (hid >= 0x04 && hid <= 0x1D) { v = 'A' + (hid - 0x04); s = 0x1E + (hid - 0x04); }
    else if (hid >= 0x1E && hid <= 0x26) { v = '1' + (hid - 0x1E); s = 0x02 + (hid - 0x1E); }
    else if (hid == 0x27) { v = '0'; s = 0x0B; }
    else if (hid >= 0x3A && hid <= 0x45) { v = 0x70 + (hid - 0x3A); s = 0x3B + (hid - 0x3A); }
    else switch (hid) {
    case 0x28: v = 0x0D; s = 0x1C; break;   /* return */
    case 0x29: v = 0x1B; s = 0x01; break;   /* escape */
    case 0x2A: v = 0x08; s = 0x0E; break;   /* backspace */
    case 0x2B: v = 0x09; s = 0x0F; break;   /* tab */
    case 0x2C: v = 0x20; s = 0x39; break;   /* space */
    case 0x2D: v = 0xBD; s = 0x0C; break;   /* minus */
    case 0x2E: v = 0xBB; s = 0x0D; break;   /* equals */
    case 0x2F: v = 0xDB; s = 0x1A; break;   /* left bracket */
    case 0x30: v = 0xDD; s = 0x1B; break;   /* right bracket */
    case 0x31: v = 0xDC; s = 0x2B; break;   /* backslash */
    case 0x32: v = 0xBF; s = 0x2D; break;   /* non-US hash */
    case 0x33: v = 0xBA; s = 0x27; break;   /* semicolon */
    case 0x34: v = 0xDE; s = 0x28; break;   /* apostrophe */
    case 0x35: v = 0xC0; s = 0x29; break;   /* grave */
    case 0x36: v = 0xBC; s = 0x33; break;   /* comma */
    case 0x37: v = 0xBE; s = 0x34; break;   /* period */
    case 0x38: v = 0xBF; s = 0x35; break;   /* slash */
    case 0x39: v = 0x14; s = 0x3A; break;   /* caps lock */
    case 0x46: v = 0x2C; s = 0x37; break;   /* print screen */
    case 0x47: v = 0x91; s = 0x46; break;   /* scroll lock */
    case 0x48: v = 0x13; s = 0x45; break;   /* pause */
    case 0x49: v = 0x2D; s = 0x52; break;   /* insert */
    case 0x4A: v = 0x2E; s = 0x53; break;   /* delete */
    case 0x4B: v = 0x24; s = 0x47; break;   /* home */
    case 0x4C: v = 0x23; s = 0x4F; break;   /* end */
    case 0x4D: v = 0x21; s = 0x49; break;   /* page up */
    case 0x4E: v = 0x22; s = 0x51; break;   /* page down */
    case 0x4F: v = 0x27; s = 0x4D; break;   /* right */
    case 0x50: v = 0x25; s = 0x4B; break;   /* left */
    case 0x51: v = 0x28; s = 0x50; break;   /* down */
    case 0x52: v = 0x26; s = 0x48; break;   /* up */
    case 0x53: v = 0x90; s = 0x45; break;   /* num lock */
    case 0x58: v = 0x0D; s = 0x1C; break;   /* keypad enter */
    case 0x59: v = 0xA3; s = 0x1D; break;   /* right control */
    case 0x5A: v = 0x6A; s = 0x37; break;   /* keypad asterisk */
    case 0x5B: v = 0x6B; s = 0x4E; break;   /* keypad plus */
    case 0x5C: v = 0x6C; s = 0x53; break;   /* keypad comma */
    case 0x5D: v = 0x6D; s = 0x4B; break;   /* keypad minus */
    case 0x5E: v = 0x6E; s = 0x52; break;   /* keypad period */
    case 0x5F: v = 0x6F; s = 0x35; break;   /* keypad slash */
    case 0xE0: v = 0xA2; s = 0x1D; break;   /* left control */
    case 0xE1: v = 0xA0; s = 0x2A; break;   /* left shift */
    case 0xE2: v = 0xA4; s = 0x38; break;   /* left alt */
    case 0xE3: v = 0x5B; s = 0x5B; break;   /* left command */
    case 0xE4: v = 0xA3; s = 0x1D; break;   /* right control */
    case 0xE5: v = 0xA1; s = 0x36; break;   /* right shift */
    case 0xE6: v = 0xA5; s = 0x38; break;   /* right alt */
    case 0xE7: v = 0x5C; s = 0x5C; break;   /* right command */
    default: break;
    }

    if (vk) *vk = v;
    if (scan) *scan = s;
    return v != 0;
}
