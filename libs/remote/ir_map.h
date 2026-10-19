/* ---- scancode -> keycode table -------------------------------------- */

struct scancode_map {
    uint8_t  scancode;
    uint16_t keycode;
    const char *name; /* for logging only */
};

// Recovery bits:
#ifdef BUILD_RECOVERY
#undef KEY_SELECT
#define KEY_SELECT KEY_ENTER
#undef KEY_BACK
#define KEY_BACK KEY_BACKSPACE
#endif

/* Probably complete, but the keycode selection is a bit questionable.
 * Best to check the stock firmware later on. */
static const struct scancode_map kScancodeMap[] = {
    // Cheapo remote:
    { 0x00, KEY_0, "0" },
    { 0x01, KEY_1, "1" },
    { 0x02, KEY_2, "2" },
    { 0x03, KEY_3, "3" },
    { 0x04, KEY_4, "4" },
    { 0x05, KEY_5, "5" },
    { 0x06, KEY_6, "6" },
    { 0x07, KEY_7, "7" },
    { 0x08, KEY_8, "8" },
    { 0x09, KEY_9, "9" },
    { 0x0a, KEY_DIGITS, "digits" },
    { 0x0b, KEY_CHANNELUP, "channel up" },
    { 0x0c, KEY_VOLUMEUP, "volume up" },
    { 0x0d, KEY_PAGEUP, "page up" },
    { 0x11, KEY_GOTO, "LOC" },
    { 0x12, KEY_MUTE, "mute" },
    { 0x13, KEY_PAGEDOWN, "page down" },
    { 0x15, KEY_CHANNELDOWN, "channel down" },
    { 0x16, KEY_DELETE, "delete" },
    { 0x18, KEY_VOLUMEDOWN, "volume down" },
    { 0x1c, KEY_AUDIO, "L/R" },
    { 0x1d, KEY_SETUP, "SET" },
    { 0x1e, KEY_INFO, "info"},
    { 0x40, KEY_POWER, "power" },
    { 0x43, KEY_REWIND, "<<" },
    { 0x44, KEY_PLAYPAUSE, "play/pause" },
    { 0x45, KEY_STOP, "stop" },
    { 0x46, KEY_FASTFORWARD, ">>" },
    { 0x47, KEY_UP, "up" },
    { 0x48, KEY_LEFT, "left" },
    { 0x49, KEY_SELECT, "center" }, // Android treats this key as the center button in a d-pad.
    { 0x4a, KEY_RIGHT, "right" },
    { 0x4b, KEY_DOWN, "down" },
    { 0x4c, KEY_BACK, "back" },
    { 0x4d, KEY_MENU, "menu" },
    { 0x4e, KEY_F1, "F1" },
    { 0x4f, KEY_F2, "F2" },
    { 0x50, KEY_F3, "F3" },
    { 0x51, KEY_F4, "F4" },
    { 0x56, KEY_HOMEPAGE, "home" },

    // ZTE B866F remote:
    { 0xdc, KEY_POWER, "power" },
    { 0x80, KEY_VOLUMEUP, "volume up" },
    { 0x9c, KEY_MUTE, "mute" },
    { 0xea, KEY_AUDIO, "L/R" },
    { 0x81, KEY_VOLUMEDOWN, "volume down" },
    { 0x95, KEY_PLAYPAUSE, "play/pause" },
    { 0xeb, KEY_TV, "TVOD" },
    { 0xe3, KEY_F7, "netflix" },
    { 0xe9, KEY_APPSELECT, "apps" },
    { 0xfe, 582, "assistant" },
    { 0xca, KEY_UP, "up" },
    { 0x99, KEY_LEFT, "left" },
    { 0xc1, KEY_RIGHT, "right" },
    { 0xd2, KEY_DOWN, "down" },
    { 0xce, KEY_SELECT, "center" }, // Android treats this key as the center button in a d-pad.
    { 0xc5, KEY_BACK, "back" },
    { 0x82, KEY_HOMEPAGE, "home" },
    { 0x88, KEY_F8, "dot-bigdot-dot" },
    { 0x92, KEY_1, "1" },
    { 0x93, KEY_2, "2" },
    { 0xcc, KEY_3, "3" },
    { 0x8e, KEY_4, "4" },
    { 0x8f, KEY_5, "5" },
    { 0xc8, KEY_6, "6" },
    { 0x8a, KEY_7, "7" },
    { 0x8b, KEY_8, "8" },
    { 0xc4, KEY_9, "9" },
    { 0xde, KEY_DOT, "dot" },
    { 0x87, KEY_0, "0" },
    { 0xdf, KEY_BACKSPACE, "backspace" },
};
#define kScancodeMapLen (sizeof(kScancodeMap) / sizeof(kScancodeMap[0]))