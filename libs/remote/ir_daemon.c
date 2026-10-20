/*
 * ir_daemon.c
 *
 * Reads fixed-size 8-byte scancode records from /dev/lirc0 and turns
 * them into real Linux key events via /dev/uinput.
 *
 * Record layout, reverse engineered from hex dump:
 *
 *   offset 0x0: u32 scancode   (only low byte is meaningful)
 *   offset 0x4: u32 keycode
 *
 * Each physical press produces two identical 8-byte records. There is
 * no "release" record: a release has to be inferred from a gap in the
 * incoming records, so this daemon runs a debounce/hold timer:
 *
 *   - first record for a scancode -> emit KEY press
 *   - further records with the same scancode before the timeout ->
 *     just refresh the timer (button still held)
 *   - a different scancode arrives -> release old key, press new key
 *   - timeout elapses with no new record -> emit KEY release
 *
 * Usage:
 *
 *   ir_daemon [--mouse-emu]
 *
 * With --mouse-emu (useful for TWRP), the d-pad stops producing key
 * events and instead acts as a mouse:
 *
 *   - up/down/left/right move the pointer (REL_X / REL_Y). Movement is
 *     driven by a timer while the button is held and accelerates the
 *     longer it is held.
 *   - center acts as the left mouse button (BTN_LEFT).
 *
 * All other keys (back, home, menu, digits, ...) keep working as normal
 * key events.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "ir_map.h"

/* ---- configuration ------------------------------------------------ */

#define LIRC_DEVICE       "/dev/lirc0"
#define UINPUT_DEVICE     "/dev/uinput"
#define DEVICE_NAME       "nec_remote_virtual"
#define CUSTOM_MAP_PATH   "/vendor/etc/ir_custom.conf"
#define CUSTOM_NAME_MAX   64
#define CUSTOM_LINE_MAX   256

/* How long to wait, in milliseconds, after the last record for a given
 * scancode before deciding the button has been released. */
#define RELEASE_TIMEOUT_MS   150

/* Mouse emulation tuning. While a d-pad direction is held, the pointer
 * is moved every MOUSE_TICK_MS. The step (pixels per tick) starts at
 * MOUSE_STEP_MIN and grows by 1 every MOUSE_ACCEL_MS of holding, up to
 * MOUSE_STEP_MAX. */
#define MOUSE_TICK_MS        16
#define MOUSE_ACCEL_MS       150
#define MOUSE_STEP_MIN       1
#define MOUSE_STEP_MAX       20

/* ---- wire format ---------------------------------------------------- */

struct ir_record {
    uint32_t scancode;
    uint32_t keycode;      // The keycodes the kernel sends are usually invalid.
} __attribute__((packed));

/* Custom mappings are additive: an entry can only claim a scancode that
 * is not already present in ir_map.h. The map is loaded once at startup. */
struct custom_scancode_entry {
    struct scancode_map mapping;
    char name[CUSTOM_NAME_MAX];
};

static struct custom_scancode_entry g_custom_map[256];
static size_t g_custom_map_len = 0;

struct named_keycode {
    const char *name;
    uint16_t keycode;
};

/* Common Linux input-event key names accepted by ir_custom.conf. Names may
 * be written with or without the KEY_ prefix. Numeric keycodes are accepted
 * too, so keys absent from this convenience table remain usable. */
static const struct named_keycode kNamedKeycodes[] = {
    { "KEY_0", KEY_0 }, { "KEY_1", KEY_1 }, { "KEY_2", KEY_2 },
    { "KEY_3", KEY_3 }, { "KEY_4", KEY_4 }, { "KEY_5", KEY_5 },
    { "KEY_6", KEY_6 }, { "KEY_7", KEY_7 }, { "KEY_8", KEY_8 },
    { "KEY_9", KEY_9 },
    { "KEY_A", KEY_A }, { "KEY_B", KEY_B }, { "KEY_C", KEY_C },
    { "KEY_D", KEY_D }, { "KEY_E", KEY_E }, { "KEY_F", KEY_F },
    { "KEY_G", KEY_G }, { "KEY_H", KEY_H }, { "KEY_I", KEY_I },
    { "KEY_J", KEY_J }, { "KEY_K", KEY_K }, { "KEY_L", KEY_L },
    { "KEY_M", KEY_M }, { "KEY_N", KEY_N }, { "KEY_O", KEY_O },
    { "KEY_P", KEY_P }, { "KEY_Q", KEY_Q }, { "KEY_R", KEY_R },
    { "KEY_S", KEY_S }, { "KEY_T", KEY_T }, { "KEY_U", KEY_U },
    { "KEY_V", KEY_V }, { "KEY_W", KEY_W }, { "KEY_X", KEY_X },
    { "KEY_Y", KEY_Y }, { "KEY_Z", KEY_Z },
    { "KEY_ENTER", KEY_ENTER }, { "KEY_ESC", KEY_ESC },
    { "KEY_BACKSPACE", KEY_BACKSPACE }, { "KEY_TAB", KEY_TAB },
    { "KEY_SPACE", KEY_SPACE }, { "KEY_MINUS", KEY_MINUS },
    { "KEY_EQUAL", KEY_EQUAL }, { "KEY_LEFTBRACE", KEY_LEFTBRACE },
    { "KEY_RIGHTBRACE", KEY_RIGHTBRACE }, { "KEY_BACKSLASH", KEY_BACKSLASH },
    { "KEY_SEMICOLON", KEY_SEMICOLON }, { "KEY_APOSTROPHE", KEY_APOSTROPHE },
    { "KEY_GRAVE", KEY_GRAVE }, { "KEY_COMMA", KEY_COMMA },
    { "KEY_DOT", KEY_DOT }, { "KEY_SLASH", KEY_SLASH },
    { "KEY_CAPSLOCK", KEY_CAPSLOCK }, { "KEY_SYSRQ", KEY_SYSRQ },
    { "KEY_SCROLLLOCK", KEY_SCROLLLOCK }, { "KEY_PAUSE", KEY_PAUSE },
    { "KEY_INSERT", KEY_INSERT }, { "KEY_HOME", KEY_HOME },
    { "KEY_PAGEUP", KEY_PAGEUP }, { "KEY_DELETE", KEY_DELETE },
    { "KEY_END", KEY_END }, { "KEY_PAGEDOWN", KEY_PAGEDOWN },
    { "KEY_RIGHT", KEY_RIGHT }, { "KEY_LEFT", KEY_LEFT },
    { "KEY_DOWN", KEY_DOWN }, { "KEY_UP", KEY_UP },
    { "KEY_NUMLOCK", KEY_NUMLOCK },
    { "KEY_KP0", KEY_KP0 }, { "KEY_KP1", KEY_KP1 },
    { "KEY_KP2", KEY_KP2 }, { "KEY_KP3", KEY_KP3 },
    { "KEY_KP4", KEY_KP4 }, { "KEY_KP5", KEY_KP5 },
    { "KEY_KP6", KEY_KP6 }, { "KEY_KP7", KEY_KP7 },
    { "KEY_KP8", KEY_KP8 }, { "KEY_KP9", KEY_KP9 },
    { "KEY_KPDOT", KEY_KPDOT }, { "KEY_KPENTER", KEY_KPENTER },
    { "KEY_KPPLUS", KEY_KPPLUS }, { "KEY_KPMINUS", KEY_KPMINUS },
    { "KEY_KPASTERISK", KEY_KPASTERISK }, { "KEY_KPSLASH", KEY_KPSLASH },
    { "KEY_F1", KEY_F1 }, { "KEY_F2", KEY_F2 },
    { "KEY_F3", KEY_F3 }, { "KEY_F4", KEY_F4 },
    { "KEY_F5", KEY_F5 }, { "KEY_F6", KEY_F6 },
    { "KEY_F7", KEY_F7 }, { "KEY_F8", KEY_F8 },
    { "KEY_F9", KEY_F9 }, { "KEY_F10", KEY_F10 },
    { "KEY_F11", KEY_F11 }, { "KEY_F12", KEY_F12 },
    { "KEY_POWER", KEY_POWER }, { "KEY_MUTE", KEY_MUTE },
    { "KEY_VOLUMEUP", KEY_VOLUMEUP }, { "KEY_VOLUMEDOWN", KEY_VOLUMEDOWN },
    { "KEY_PLAYPAUSE", KEY_PLAYPAUSE }, { "KEY_STOP", KEY_STOP },
    { "KEY_REWIND", KEY_REWIND }, { "KEY_FASTFORWARD", KEY_FASTFORWARD },
    { "KEY_PLAY", KEY_PLAY }, { "KEY_RECORD", KEY_RECORD },
    { "KEY_NEXTSONG", KEY_NEXTSONG }, { "KEY_PREVIOUSSONG", KEY_PREVIOUSSONG },
    { "KEY_MENU", KEY_MENU }, { "KEY_BACK", KEY_BACK },
    { "KEY_SELECT", KEY_SELECT }, { "KEY_OK", KEY_OK },
    { "KEY_HOMEPAGE", KEY_HOMEPAGE }, { "KEY_SEARCH", KEY_SEARCH },
    { "KEY_INFO", KEY_INFO }, { "KEY_SETUP", KEY_SETUP },
    { "KEY_TV", KEY_TV }, { "KEY_AUDIO", KEY_AUDIO },
    { "KEY_SUBTITLE", KEY_SUBTITLE }, { "KEY_TEXT", KEY_TEXT },
    { "KEY_EPG", KEY_EPG }, { "KEY_CHANNEL", KEY_CHANNEL },
    { "KEY_RED", KEY_RED }, { "KEY_GREEN", KEY_GREEN },
    { "KEY_YELLOW", KEY_YELLOW }, { "KEY_BLUE", KEY_BLUE },
    { "KEY_FAVORITES", KEY_FAVORITES },
    { "KEY_DIGITS", KEY_DIGITS }, { "KEY_CHANNELUP", KEY_CHANNELUP },
    { "KEY_CHANNELDOWN", KEY_CHANNELDOWN }, { "KEY_GOTO", KEY_GOTO },
    { "KEY_APPSELECT", KEY_APPSELECT }, { "KEY_F13", KEY_F13 },
    { "KEY_F14", KEY_F14 }, { "KEY_F15", KEY_F15 },
    { "KEY_F16", KEY_F16 }, { "KEY_F17", KEY_F17 },
    { "KEY_F18", KEY_F18 }, { "KEY_F19", KEY_F19 },
    { "KEY_F20", KEY_F20 }, { "KEY_F21", KEY_F21 },
    { "KEY_F22", KEY_F22 }, { "KEY_F23", KEY_F23 },
    { "KEY_F24", KEY_F24 },
    { "BTN_LEFT", BTN_LEFT }, { "BTN_RIGHT", BTN_RIGHT },
    { "BTN_MIDDLE", BTN_MIDDLE },
};

static int is_builtin_scancode(uint8_t scancode) {
    for (size_t i = 0; i < kScancodeMapLen; i++) {
        if (kScancodeMap[i].scancode == scancode) return 1;
    }
    return 0;
}

static int parse_unsigned_number(const char *text, unsigned long *value_out) {
    char *end = NULL;
    int base = 10;
    unsigned long value;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) base = 16;
    errno = 0;
    value = strtoul(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') return -1;
    *value_out = value;
    return 0;
}

static int named_keycode(const char *name, uint16_t *keycode_out) {
    char with_prefix[ CUSTOM_NAME_MAX ];
    const char *candidate = name;

    for (size_t i = 0; i < sizeof(kNamedKeycodes) / sizeof(kNamedKeycodes[0]); i++) {
        if (strcmp(candidate, kNamedKeycodes[i].name) == 0) {
            *keycode_out = kNamedKeycodes[i].keycode;
            return 0;
        }
    }

    if (strncmp(name, "KEY_", 4) != 0 && strlen(name) + 4 < sizeof(with_prefix)) {
        snprintf(with_prefix, sizeof(with_prefix), "KEY_%s", name);
        candidate = with_prefix;
        for (size_t i = 0; i < sizeof(kNamedKeycodes) / sizeof(kNamedKeycodes[0]); i++) {
            if (strcmp(candidate, kNamedKeycodes[i].name) == 0) {
                *keycode_out = kNamedKeycodes[i].keycode;
                return 0;
            }
        }
    }
    return -1;
}

static int parse_keycode(const char *text, uint16_t *keycode_out) {
    unsigned long value;
    if (parse_unsigned_number(text, &value) != 0) {
        if (named_keycode(text, keycode_out) != 0) return -1;
        value = *keycode_out;
    }
    if (value == KEY_RESERVED || value > KEY_MAX) return -1;
    *keycode_out = (uint16_t)value;
    return 0;
}

static int custom_has_scancode(uint8_t scancode) {
    for (size_t i = 0; i < g_custom_map_len; i++) {
        if (g_custom_map[i].mapping.scancode == scancode) return 1;
    }
    return 0;
}

static void load_custom_map(void) {
    FILE *fp = fopen(CUSTOM_MAP_PATH, "r");
    if (!fp) {
        if (errno == ENOENT) {
            syslog(LOG_INFO, "no custom map at %s; using built-in mappings only",
                   CUSTOM_MAP_PATH);
        } else {
            syslog(LOG_WARNING, "cannot open custom map %s: %s; using built-in mappings only",
                   CUSTOM_MAP_PATH, strerror(errno));
        }
        return;
    }

    char line[CUSTOM_LINE_MAX];
    unsigned long line_number = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_number++;
        if (!strchr(line, '\n') && !feof(fp)) {
            int ch;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF) {}
            syslog(LOG_WARNING, "%s:%lu: line too long; ignoring",
                   CUSTOM_MAP_PATH, line_number);
            continue;
        }

        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *cursor = line;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0') continue;

        char *saveptr = NULL;
        char *scancode_text = strtok_r(cursor, " \t\r\n", &saveptr);
        char *keycode_text = strtok_r(NULL, " \t\r\n", &saveptr);
        if (!scancode_text || !keycode_text) {
            syslog(LOG_WARNING, "%s:%lu: expected '<scancode> <keycode> [label]'; ignoring",
                   CUSTOM_MAP_PATH, line_number);
            continue;
        }

        unsigned long scancode_value;
        uint16_t keycode;
        if (parse_unsigned_number(scancode_text, &scancode_value) != 0 ||
            scancode_value > 0xff) {
            syslog(LOG_WARNING, "%s:%lu: invalid 8-bit scancode '%s'; ignoring",
                   CUSTOM_MAP_PATH, line_number, scancode_text);
            continue;
        }
        if (parse_keycode(keycode_text, &keycode) != 0) {
            syslog(LOG_WARNING, "%s:%lu: invalid keycode '%s'; use a known KEY_* name or numeric code; ignoring",
                   CUSTOM_MAP_PATH, line_number, keycode_text);
            continue;
        }

        uint8_t scancode = (uint8_t)scancode_value;
        if (is_builtin_scancode(scancode)) {
            syslog(LOG_WARNING,
                   "%s:%lu: scancode 0x%02x is built in and cannot be overridden; ignoring",
                   CUSTOM_MAP_PATH, line_number, scancode);
            continue;
        }
        if (custom_has_scancode(scancode)) {
            syslog(LOG_WARNING,
                   "%s:%lu: duplicate custom scancode 0x%02x; first mapping wins",
                   CUSTOM_MAP_PATH, line_number, scancode);
            continue;
        }
        if (g_custom_map_len >= sizeof(g_custom_map) / sizeof(g_custom_map[0])) {
            syslog(LOG_WARNING, "%s:%lu: custom map is full; ignoring entry",
                   CUSTOM_MAP_PATH, line_number);
            continue;
        }

        struct custom_scancode_entry *entry = &g_custom_map[g_custom_map_len++];
        entry->mapping.scancode = scancode;
        entry->mapping.keycode = keycode;
        char *label = saveptr;
        while (label && *label && isspace((unsigned char)*label)) label++;
        if (label && *label) {
            size_t label_len = strlen(label);
            while (label_len > 0 && isspace((unsigned char)label[label_len - 1]))
                label[--label_len] = '\0';
            snprintf(entry->name, sizeof(entry->name), "%s", label);
        } else {
            snprintf(entry->name, sizeof(entry->name), "%s", keycode_text);
        }
        entry->mapping.name = entry->name;
        syslog(LOG_INFO, "custom mapping: scancode 0x%02x -> keycode %u (%s)",
               scancode, keycode, entry->name);
    }

    if (ferror(fp)) {
        syslog(LOG_WARNING, "error while reading custom map %s: %s",
               CUSTOM_MAP_PATH, strerror(errno));
    }
    fclose(fp);
    syslog(LOG_INFO, "loaded %zu custom scancode mapping(s) from %s",
           g_custom_map_len, CUSTOM_MAP_PATH);
}

static int raw_to_keycode(uint8_t raw, const char **name_out) {
    /* Built-in mappings always win, even if a config file is malformed or
     * attempts to define the same scancode. */
    for (size_t i = 0; i < kScancodeMapLen; i++) {
        if (kScancodeMap[i].scancode == raw) {
            if (name_out) *name_out = kScancodeMap[i].name;
            return kScancodeMap[i].keycode;
        }
    }
    for (size_t i = 0; i < g_custom_map_len; i++) {
        if (g_custom_map[i].mapping.scancode == raw) {
            if (name_out) *name_out = g_custom_map[i].mapping.name;
            return g_custom_map[i].mapping.keycode;
        }
    }
    return -1;
}

/* ---- mouse emulation helpers ----------------------------------------- */

/* Runtime flag set from the command line. */
static int g_mouse_emu = 0;

/* If keycode is a d-pad direction, fill in the unit direction vector
 * and return 1. Otherwise return 0. */
static int mouse_direction(int keycode, int *dx, int *dy) {
    switch (keycode) {
    case KEY_UP:    *dx =  0; *dy = -1; return 1;
    case KEY_DOWN:  *dx =  0; *dy =  1; return 1;
    case KEY_LEFT:  *dx = -1; *dy =  0; return 1;
    case KEY_RIGHT: *dx =  1; *dy =  0; return 1;
    default:        return 0;
    }
}

/* Keys that get consumed by mouse emulation (d-pad + center). */
static int is_mouse_key(int keycode) {
    int dx, dy;
    return mouse_direction(keycode, &dx, &dy) || keycode == KEY_SELECT;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- uinput setup ---------------------------------------------------- */

static int uinput_open_and_register(void) {
    int fd = open(UINPUT_DEVICE, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        syslog(LOG_ERR, "open(%s) failed: %s", UINPUT_DEVICE, strerror(errno));
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto ioctl_fail;

    for (size_t i = 0; i < kScancodeMapLen; i++) {
        int keycode = kScancodeMap[i].keycode;
        /* In mouse mode the d-pad and center never come out as keys. */
        if (g_mouse_emu && is_mouse_key(keycode)) continue;
        if (ioctl(fd, UI_SET_KEYBIT, keycode) < 0)
            goto ioctl_fail;
    }
    /* Custom mappings must be registered with uinput as well as translated
     * in raw_to_keycode(), or the kernel may reject their key events. */
    for (size_t i = 0; i < g_custom_map_len; i++) {
        int keycode = g_custom_map[i].mapping.keycode;
        if (g_mouse_emu && is_mouse_key(keycode)) continue;
        if (ioctl(fd, UI_SET_KEYBIT, keycode) < 0)
            goto ioctl_fail;
    }

    if (g_mouse_emu) {
        if (ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) < 0) goto ioctl_fail;
        if (ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) < 0) goto ioctl_fail;
        if (ioctl(fd, UI_SET_EVBIT, EV_REL) < 0) goto ioctl_fail;
        if (ioctl(fd, UI_SET_RELBIT, REL_X) < 0) goto ioctl_fail;
        if (ioctl(fd, UI_SET_RELBIT, REL_Y) < 0) goto ioctl_fail;
        /* Real USB mice send MSC_SCAN before every button event. TWRP's
         * vk_modify() relies on a non-SYN event arriving between two
         * SYN_REPORTs; without it the SYN after a click is turned into a
         * bogus touch release. See uinput_click(). */
        if (ioctl(fd, UI_SET_EVBIT, EV_MSC) < 0) goto ioctl_fail;
        if (ioctl(fd, UI_SET_MSCBIT, MSC_SCAN) < 0) goto ioctl_fail;
    }

    /* Kernel 3.4 predates UI_DEV_SETUP/UI_DEV_CREATE-from-ioctl (added
     * around kernel 4.5). Use the legacy interface instead: fill in a
     * struct uinput_user_dev and write() it, then UI_DEV_CREATE. */
    struct uinput_user_dev uudev;
    memset(&uudev, 0, sizeof(uudev));
    strncpy(uudev.name, DEVICE_NAME, sizeof(uudev.name) - 1);
    uudev.id.bustype = BUS_VIRTUAL;
    uudev.id.vendor  = 0x0B;   /* matches PRODUCT= vendor field seen in uevent */
    uudev.id.product = 0x0B;
    uudev.id.version = 1;

    if (write(fd, &uudev, sizeof(uudev)) != sizeof(uudev)) {
        syslog(LOG_ERR, "uinput_user_dev write failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) goto ioctl_fail;

    /* Give the kernel/udev a moment to finish registering the node
     * before anything tries to open it. */
    usleep(100 * 1000);

    return fd;

ioctl_fail:
    syslog(LOG_ERR, "uinput setup ioctl failed: %s", strerror(errno));
    close(fd);
    return -1;
}

static void uinput_emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) {
        syslog(LOG_WARNING, "uinput write failed: %s", strerror(errno));
    }
}

static void uinput_key(int fd, uint16_t keycode, int down) {
    uinput_emit(fd, EV_KEY, keycode, down ? 1 : 0);
    uinput_emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void uinput_rel(int fd, int dx, int dy) {
    if (dx) uinput_emit(fd, EV_REL, REL_X, dx);
    if (dy) uinput_emit(fd, EV_REL, REL_Y, dy);
    uinput_emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* Mouse button 1 press/release, framed like a real HID mouse does it:
 * MSC_SCAN, then the button, then SYN_REPORT. */
static void uinput_click(int fd, int down) {
    uinput_emit(fd, EV_MSC, MSC_SCAN, 0x90001);
    uinput_key(fd, BTN_LEFT, down);
}

/* ---- held-key state ---------------------------------------------------- */

struct hold_state {
    int     keycode;        /* currently-held key, or -1 if none */
    int     is_dir;         /* mouse mode: held key is a d-pad direction */
    int     dx, dy;         /* mouse mode: unit direction vector */
    int64_t press_ms;       /* when the current key was first pressed */
    int64_t last_rec_ms;    /* when the last record for it arrived */
    int64_t next_tick_ms;   /* mouse mode: when to move the pointer next */
};

static void mouse_move(int fd, const struct hold_state *h, int64_t now) {
    int step = MOUSE_STEP_MIN + (int)((now - h->press_ms) / MOUSE_ACCEL_MS);
    if (step > MOUSE_STEP_MAX) step = MOUSE_STEP_MAX;
    uinput_rel(fd, h->dx * step, h->dy * step);
}

static void hold_release(int fd, struct hold_state *h) {
    if (h->keycode < 0) return;
    if (g_mouse_emu && h->keycode == KEY_SELECT) {
        uinput_click(fd, 0);
    } else if (!h->is_dir) {
        uinput_key(fd, h->keycode, 0);
    }
    /* Direction keys in mouse mode have nothing to release. */
    h->keycode = -1;
    h->is_dir = 0;
}

static void hold_press(int fd, struct hold_state *h, int keycode, int64_t now) {
    h->keycode = keycode;
    h->is_dir = 0;
    h->press_ms = now;
    h->last_rec_ms = now;

    if (g_mouse_emu) {
        int dx, dy;
        if (mouse_direction(keycode, &dx, &dy)) {
            h->is_dir = 1;
            h->dx = dx;
            h->dy = dy;
            mouse_move(fd, h, now);               /* immediate first nudge */
            h->next_tick_ms = now + MOUSE_TICK_MS;
            return;
        }
        if (keycode == KEY_SELECT) {
            uinput_click(fd, 1);
            return;
        }
    }
    uinput_key(fd, keycode, 1);
}

/* ---- main loop -------------------------------------------------------- */

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [--mouse-emu]\n"
        "\n"
        "  --mouse-emu   d-pad moves a mouse pointer, center is left click\n"
        "\n"
        "  Optional additive mappings are read from " CUSTOM_MAP_PATH "\n",
        argv0);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mouse-emu") == 0) {
            g_mouse_emu = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    openlog("ir_daemon", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "starting, reading %s%s", LIRC_DEVICE,
           g_mouse_emu ? " (mouse emulation on)" : "");
    load_custom_map();

    int lirc_fd = open(LIRC_DEVICE, O_RDONLY);
    if (lirc_fd < 0) {
        syslog(LOG_ERR, "open(%s) failed: %s", LIRC_DEVICE, strerror(errno));
        return 1;
    }

    int uinput_fd = uinput_open_and_register();
    if (uinput_fd < 0) {
        close(lirc_fd);
        return 1;
    }

    struct hold_state hold;
    memset(&hold, 0, sizeof(hold));
    hold.keycode = -1;

    struct pollfd pfd;
    pfd.fd = lirc_fd;
    pfd.events = POLLIN;

    for (;;) {
        /* Work out how long we can sleep: until the release deadline or
         * (while moving the pointer) the next movement tick, whichever
         * comes first. */
        int64_t now = now_ms();
        int64_t deadline = -1;
        if (hold.keycode >= 0) {
            deadline = hold.last_rec_ms + RELEASE_TIMEOUT_MS;
            if (hold.is_dir && hold.next_tick_ms < deadline)
                deadline = hold.next_tick_ms;
        }
        int timeout = deadline < 0 ? -1
                    : deadline > now ? (int)(deadline - now) : 0;

        int ret = poll(&pfd, 1, timeout);

        if (ret < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "poll failed: %s", strerror(errno));
            break;
        }

        now = now_ms();

        if (ret > 0) {
            struct ir_record rec;
            ssize_t n = read(lirc_fd, &rec, sizeof(rec));
            if (n < 0) {
                if (errno == EINTR) continue;
                syslog(LOG_ERR, "read failed: %s", strerror(errno));
                break;
            }
            if (n != (ssize_t)sizeof(rec)) {
                /* Partial/unexpected read. Log and resync by discarding.
                 * Shouldn't normally happen since the driver writes fixed
                 * 8-byte records, but don't trust that blindly. */
                syslog(LOG_WARNING, "short read: got %zd bytes, expected %zu",
                       n, sizeof(rec));
                continue;
            }

            uint8_t scancode = rec.scancode & 0xff;
            const char *name = NULL;
            int keycode = raw_to_keycode(scancode, &name);

            if (keycode < 0) {
                syslog(LOG_DEBUG, "unmapped scancode=0x%02x, ignoring", scancode);
                continue;
            }

            if (hold.keycode == keycode) {
                /* Same button still being reported: just refresh the
                 * release timer. */
                hold.last_rec_ms = now;
            } else {
                /* Different (or first) scancode: release whatever was
                 * held, then press the new one. */
                hold_release(uinput_fd, &hold);
                hold_press(uinput_fd, &hold, keycode, now);
                syslog(LOG_DEBUG,
                    "raw scancode=0x%08x keycode=0x%08x (%s)",
                    (unsigned)rec.scancode,
                    (unsigned)rec.keycode,
                    name ? name : "?");
            }
        }

        /* Timers: release detection and pointer movement. */
        if (hold.keycode >= 0) {
            if (now - hold.last_rec_ms >= RELEASE_TIMEOUT_MS) {
                hold_release(uinput_fd, &hold);
            } else if (hold.is_dir && now >= hold.next_tick_ms) {
                mouse_move(uinput_fd, &hold, now);
                hold.next_tick_ms = now + MOUSE_TICK_MS;
            }
        }
    }

    hold_release(uinput_fd, &hold);
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    close(lirc_fd);
    closelog();
    return 0;
}
