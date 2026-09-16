/*
 * ir_daemon.c
 *
 * Reads fixed-size 8-byte scancode records from /dev/lirc0 and turns
 * them into real Linux key events via /dev/uinput.
 *
 * Record layout, reverse engineered from hex dump:
 *
 *   offset 0x0: u32 scancode   (only low byte is meaningful)
 *   offset 0x4: u32 field3     (constant per-button, meaning unknown)
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
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/* ---- configuration ------------------------------------------------ */

#define LIRC_DEVICE       "/dev/lirc0"
#define UINPUT_DEVICE     "/dev/uinput"
#define DEVICE_NAME       "nec_remote_virtual"

/* How long to wait, in milliseconds, after the last record for a given
 * scancode before deciding the button has been released. */
#define RELEASE_TIMEOUT_MS   150

/* ---- wire format ---------------------------------------------------- */

struct ir_record {
    uint32_t scancode;
    uint32_t field3;      /* unused for now, see header comment */
} __attribute__((packed));

/* ---- scancode -> keycode table -------------------------------------- */

struct scancode_map {
    uint8_t  scancode;
    uint16_t keycode;
    const char *name; /* for logging only */
};


/* Probably complete, but the keycode selection is a bit questionable.
 * Best to check the stock firmware later on. */
static const struct scancode_map kScancodeMap[] = {
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
    { 0x56, KEY_HOMEPAGE, "home" }
};
#define kScancodeMapLen (sizeof(kScancodeMap) / sizeof(kScancodeMap[0]))

static int raw_to_keycode(uint8_t raw, const char **name_out) {
    for (size_t i = 0; i < kScancodeMapLen; i++) {
        if (kScancodeMap[i].scancode == raw) {
            if (name_out) *name_out = kScancodeMap[i].name;
            return kScancodeMap[i].keycode;
        }
    }
    return -1;
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
        if (ioctl(fd, UI_SET_KEYBIT, kScancodeMap[i].keycode) < 0)
            goto ioctl_fail;
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

/* ---- main loop -------------------------------------------------------- */

int main(void) {
    openlog("ir_daemon", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "starting, reading %s", LIRC_DEVICE);

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

    /* State: currently-held key, or -1 if none. */
    int held_keycode = -1;
    uint8_t held_scancode = 0;

    struct pollfd pfd;
    pfd.fd = lirc_fd;
    pfd.events = POLLIN;

    for (;;) {
        int ret = poll(&pfd, 1, RELEASE_TIMEOUT_MS);

        if (ret < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "poll failed: %s", strerror(errno));
            break;
        }

        if (ret == 0) {
            /* Timeout: no record arrived within RELEASE_TIMEOUT_MS.
             * If a key was being held, this means the button was
             * released. Emit the up event. */
            if (held_keycode >= 0) {
                uinput_key(uinput_fd, held_keycode, 0);
                /* syslog(LOG_DEBUG, "release (timeout) scancode=0x%02x",
                       held_scancode); */
                held_keycode = -1;
            }
            continue;
        }

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
             * 16-byte records, but don't trust that blindly. */
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

        if (held_keycode == keycode) {
            /* Same button still being reported. No event needed. */
            continue;
        }

        /* Different (or first) scancode: release whatever was held,
         * then press the new one. */
        if (held_keycode >= 0) {
            uinput_key(uinput_fd, held_keycode, 0);
        }
        uinput_key(uinput_fd, keycode, 1);
        held_keycode = keycode;
        held_scancode = scancode;
        /*syslog(LOG_DEBUG, "press scancode=0x%02x (%s)", scancode,
               name ? name : "?");*/
    }

    if (held_keycode >= 0) {
        uinput_key(uinput_fd, held_keycode, 0);
    }
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    close(lirc_fd);
    closelog();
    return 0;
}
