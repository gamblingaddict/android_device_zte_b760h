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

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
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

static int raw_to_keycode(uint8_t raw, const char **name_out) {
    for (size_t i = 0; i < kScancodeMapLen; i++) {
        if (kScancodeMap[i].scancode == raw) {
            if (name_out) *name_out = kScancodeMap[i].name;
            return kScancodeMap[i].keycode;
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
        "  --mouse-emu   d-pad moves a mouse pointer, center is left click\n",
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
