#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>

#include "hdmitx_uapi.h"

#define DEFAULT_POLL_INTERVAL_SEC   0.10   /* fallback cadence; 100 ms */
#define DEFAULT_COOLDOWN_SEC        1      /* avoid hammering the driver, but recover quickly */
#define DEFAULT_CONFIRM_COUNT       1      /* one positive EDID result is enough */
#define DEFAULT_SETTLE_MS           500    /* short post-bring-up settle; not user-visible */
#define DEFAULT_RES_SETTLE_MS       25     /* short clock-settle delay before VIDEO_CONFIG */
#define DEFAULT_FALLBACK_RES        HDMI_VIDEO_RES_1920x1080p_60Hz
#define DEFAULT_CMDLINE_PATH        "/proc/cmdline"
#define DEFAULT_CMDLINE_KEY         "hdmi_res"
#define LOG_TAG                     "hdmi_daemon"

static volatile sig_atomic_t g_running = 1;

static double g_poll_interval_sec = DEFAULT_POLL_INTERVAL_SEC;
static int   g_cooldown_sec      = DEFAULT_COOLDOWN_SEC;
static int   g_confirm_count     = DEFAULT_CONFIRM_COUNT;
static int   g_settle_ms         = DEFAULT_SETTLE_MS;
static int   g_res_settle_ms     = DEFAULT_RES_SETTLE_MS;
static int   g_fallback_res      = DEFAULT_FALLBACK_RES;
static int   g_explicit_res      = -1;  /* -x: overrides everything if set */
static int   g_force_res_config  = 1;   /* -F to disable */
static int   g_do_bootstrap      = 1;   /* -B to disable */
static int   g_res_bump          = 1;   /* -Z to disable the boot-time bump workaround */
static int   g_verbose           = 0;
static const char *g_device_path     = HDMI_DRV_NODE;
static const char *g_switch_path     = HDMI_SWITCH_STATE_PATH;
static const char *g_res_switch_path = HDMI_RES_SWITCH_STATE_PATH;
static const char *g_cmdline_path    = DEFAULT_CMDLINE_PATH;
static const char *g_cmdline_key     = DEFAULT_CMDLINE_KEY;

/* Live ground truth: last resolution index this daemon actually saw
 * confirmed via the res_hdmi switch (-1 = not known yet this run). */
static int g_last_good_res = -1;

#define LOGI(fmt, ...) syslog(LOG_INFO, fmt "\n", ##__VA_ARGS__)
#define LOGD(fmt, ...) do { if (g_verbose) syslog(LOG_DEBUG, "[dbg] " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOGE(fmt, ...) syslog(LOG_ERR, "[err] " fmt "\n", ##__VA_ARGS__)

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* Returns the integer value in a switch-class sysfs "state" node, or
 * -1 if the node is missing/unreadable. */
static int read_switch_state(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    int value = -1;
    if (fscanf(f, "%d", &value) != 1) {
        value = -1;
    }
    fclose(f);
    return value;
}

/* Opens a switch-class "state" node for use with poll(). Returns -1
 * (not fatal) if the node doesn't exist yet - callers retry later. */
static int open_switch_fd(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOGD("open(%s) for poll failed: %s", path, strerror(errno));
    }
    return fd;
}

/* sysfs poll() semantics require the fd's read position to be rewound
 * and re-read before a subsequent poll() will correctly report
 * POLLPRI/POLLERR again for the *next* change. Do that immediately
 * after opening and immediately after every wake. */
static void rearm_switch_fd(int fd)
{
    char scratch[32];
    ssize_t n;

    if (fd < 0) {
        return;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return;
    }
    n = read(fd, scratch, sizeof(scratch) - 1);
    (void)n;
}

static int open_hdmi_device(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        LOGD("open(%s) failed: %s", path, strerror(errno));
    }
    return fd;
}

static void sleep_ms(int ms)
{
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Queries real hardware plug state via EDID readback (doesn't depend on
 * the driver's own software state). Returns 1 = plugged, 0 = not
 * plugged, -1 = query failed. */
static int query_hw_plug_state(int fd)
{
    HDMI_EDID_T edid;
    memset(&edid, 0, sizeof(edid));

    if (ioctl(fd, MTK_HDMI_GET_EDID, &edid) < 0) {
        LOGD("MTK_HDMI_GET_EDID failed: %s", strerror(errno));
        return -1;
    }

    return edid.ui1_sink_is_plug_in ? 1 : 0;
}

/* If res_hdmi reads a valid (nonzero) value, remember it as ground
 * truth for future bring-ups. The switch stores (resolution_index + 1)
 * on success, per the driver's switch_set_state(&hdmires_switch_data,
 * hdmi_reschange + 1) call - so this also picks up resolutions that
 * ended up active through some path other than this daemon. */
static void note_res_switch_value(int res_switch_value)
{
    if (res_switch_value > 0) {
        int observed = res_switch_value - 1;
        if (observed != g_last_good_res) {
            LOGI("observed working resolution %d via res_hdmi switch", observed);
        }
        g_last_good_res = observed;
    }
}

/* Accepts either a plain integer (assumed to already be an
 * HDMI_VIDEO_RESOLUTION enum value) or a handful of common
 * human-readable forms. Returns -1 if the token isn't understood -
 * callers should treat that as "source unavailable," not as 0. */
static int parse_resolution_token(const char *tok)
{
    if (!tok || !*tok) {
        return -1;
    }

    char *endptr = NULL;
    long v = strtol(tok, &endptr, 10);
    if (endptr != tok && *endptr == '\0' && v >= 0) {
        return (int)v;
    }

    static const struct { const char *name; int res; } table[] = {
        { "480p60",   HDMI_VIDEO_RES_720x480p_60Hz   },
        { "576p50",   HDMI_VIDEO_RES_720x576p_50Hz   },
        { "720p60",   HDMI_VIDEO_RES_1280x720p_60Hz  },
        { "720p50",   HDMI_VIDEO_RES_1280x720p_50Hz  },
        { "1080i60",  HDMI_VIDEO_RES_1920x1080i_60Hz },
        { "1080i50",  HDMI_VIDEO_RES_1920x1080i_50Hz },
        { "1080p30",  HDMI_VIDEO_RES_1920x1080p_30Hz },
        { "1080p25",  HDMI_VIDEO_RES_1920x1080p_25Hz },
        { "1080p24",  HDMI_VIDEO_RES_1920x1080p_24Hz },
        { "1080p23",  HDMI_VIDEO_RES_1920x1080p_23Hz },
        { "1080p29",  HDMI_VIDEO_RES_1920x1080p_29Hz },
        { "1080p60",  HDMI_VIDEO_RES_1920x1080p_60Hz },
        { "1080p50",  HDMI_VIDEO_RES_1920x1080p_50Hz },
        { NULL, -1 },
    };

    for (int i = 0; table[i].name != NULL; i++) {
        if (strcasecmp(tok, table[i].name) == 0) {
            return table[i].res;
        }
    }

    return -1;
}

/* Reads /proc/cmdline (or wherever -C points) and looks for a
 * "<key>=<value>" whitespace-separated token, mirroring the one-shot
 * boot script's own "cat /proc/cmdline | tr ' ' '\n' | grep '^KEY='"
 * logic - done directly here so this daemon doesn't depend on that
 * script (or its init trigger) having already run. */
static int query_cmdline_resolution(void)
{
    FILE *f = fopen(g_cmdline_path, "r");
    if (!f) {
        LOGD("could not open %s: %s", g_cmdline_path, strerror(errno));
        return -1;
    }

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    size_t keylen = strlen(g_cmdline_key);
    char *saveptr = NULL;
    char *tok = strtok_r(buf, " \t\r\n", &saveptr);

    while (tok) {
        if (strncmp(tok, g_cmdline_key, keylen) == 0 && tok[keylen] == '=') {
            int res = parse_resolution_token(tok + keylen + 1);
            if (res < 0) {
                LOGD("cmdline %s=%s not understood as a resolution", g_cmdline_key, tok + keylen + 1);
            }
            return res;
        }
        tok = strtok_r(NULL, " \t\r\n", &saveptr);
    }

    LOGD("no '%s=' token found in %s", g_cmdline_key, g_cmdline_path);
    return -1;
}

/* Priority chain described in the file header comment. */
static int resolve_target_resolution(void)
{
    if (g_explicit_res >= 0) {
        LOGD("using explicit resolution override %d", g_explicit_res);
        return g_explicit_res;
    }

    if (g_last_good_res >= 0) {
        LOGD("using last known-good resolution %d", g_last_good_res);
        return g_last_good_res;
    }

    int r = query_cmdline_resolution();
    if (r >= 0) {
        LOGI("%s (cmdline %s=) -> %d", g_cmdline_path, g_cmdline_key, r);
        return r;
    }

    LOGI("no resolution source available yet, falling back to default %d", g_fallback_res);
    return g_fallback_res;
}

/* Picks a resolution guaranteed to differ from target_res, for the
 * boot-time hdmi_reschange bump workaround (see file header). Which
 * decoy we pick doesn't matter beyond "not equal to target" - it's
 * discarded within milliseconds by the real config call that follows. */
static int pick_bump_decoy(int target_res)
{
    if (target_res != HDMI_VIDEO_RES_720x480p_60Hz) {
        return HDMI_VIDEO_RES_720x480p_60Hz;
    }
    return HDMI_VIDEO_RES_1280x720p_60Hz;
}

static void force_video_config(int fd, int target_res)
{
    /* Avoids a transient clock/mutex race seen as "IRQ: RDMA0
     * abnormal" + clock-tree warnings when issued too soon after
     * enabling. Self-recovers even without this, but the delay
     * avoids the noise. */
    sleep_ms(g_res_settle_ms);

    LOGI("forcing video resolution config (res=%d)", target_res);
    if (ioctl(fd, MTK_HDMI_VIDEO_CONFIG, target_res) < 0) {
        LOGE("MTK_HDMI_VIDEO_CONFIG(%d) failed: %s", target_res, strerror(errno));
    } else {
        LOGD("MTK_HDMI_VIDEO_CONFIG(%d) ok", target_res);
    }
}

/* Full bring-up sequence: enable audio+video, power on, and (unless
 * disabled) force a resolution config to actually kick the DPI/RDMA
 * pipeline. Every step is safe to call redundantly - the driver
 * early-returns / no-ops when already in the requested state.
 *
 * allow_res_bump should be true only for this daemon's very first
 * bring-up attempt in a given boot (see the hdmi_reschange no-op bug
 * described in the file header) - once we've completed one real
 * bring-up, the kernel's own resolution cache is definitely correct
 * and the bump is just unnecessary flicker. */
static void bring_up_hdmi(int fd, const char *reason, int allow_res_bump)
{
    LOGI("bringing up HDMI pipeline (%s)", reason);

    if (ioctl(fd, MTK_HDMI_POWER_ENABLE, 1) < 0) {
        LOGD("MTK_HDMI_POWER_ENABLE(1) failed: %s", strerror(errno));
    } else {
        LOGD("MTK_HDMI_POWER_ENABLE(1) ok");
    }

    if (ioctl(fd, MTK_HDMI_AUDIO_VIDEO_ENABLE, 1) < 0) {
        LOGE("MTK_HDMI_AUDIO_VIDEO_ENABLE(1) failed: %s", strerror(errno));
    } else {
        LOGD("MTK_HDMI_AUDIO_VIDEO_ENABLE(1) ok");
    }

    if (ioctl(fd, MTK_HDMI_AUDIO_ENABLE, 1) < 0) {
        LOGD("MTK_HDMI_AUDIO_ENABLE(1) failed: %s", strerror(errno));
    } else {
        LOGD("MTK_HDMI_AUDIO_ENABLE(1) ok");
    }

    if (g_force_res_config) {
        int target_res = resolve_target_resolution();

        if (allow_res_bump && g_res_bump && g_last_good_res < 0) {
            int decoy = pick_bump_decoy(target_res);
            LOGI("first bring-up: bumping through res=%d before target res=%d "
                 "(avoids a stale-hdmi_reschange no-op leaving res_hdmi at 0)",
                 decoy, target_res);
            force_video_config(fd, decoy);
        }

        force_video_config(fd, target_res);
    }

    int sw_state  = read_switch_state(g_switch_path);
    int res_state = read_switch_state(g_res_switch_path);
    note_res_switch_value(res_state);
    LOGI("post bring-up: hdmi switch=%d, res_hdmi switch=%d", sw_state, res_state);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -d PATH   hdmitx device node             (default: %s)\n"
        "  -s PATH   \"hdmi\" switch sysfs node        (default: %s)\n"
        "  -w PATH   \"res_hdmi\" switch sysfs node    (default: %s)\n"
        "  -i SEC    fallback poll interval; fractional seconds allowed (default: %.2f)\n"
        "  -c SEC    cooldown between recovery attempts (default: %d)\n"
        "  -n COUNT  consecutive confirmations before acting (default: %d)\n"
        "  -m MS     settle time after bring-up before rechecking (default: %d)\n"
        "  -t MS     delay before forcing resolution config (default: %d)\n"
        "  -C PATH   cmdline file to read for boot resolution (default: %s)\n"
        "  -K KEY    cmdline key to look for (default: %s)\n"
        "  -r RES    fallback resolution if no other source is available\n"
        "            (default: %d = 1920x1080p60)\n"
        "  -x RES    force this exact resolution always, skip all other\n"
        "            sources (debug/testing only)\n"
        "  -F        don't force a resolution config during bring-up\n"
        "  -B        skip the one-time boot bring-up (assume something else\n"
        "            already enables/configures HDMI)\n"
        "  -Z        disable the first-boot hdmi_reschange bump workaround\n"
        "  -v        verbose logging\n"
        "  -h        show this help\n",
        argv0, HDMI_DRV_NODE, HDMI_SWITCH_STATE_PATH, HDMI_RES_SWITCH_STATE_PATH,
        DEFAULT_POLL_INTERVAL_SEC, DEFAULT_COOLDOWN_SEC, DEFAULT_CONFIRM_COUNT,
        DEFAULT_SETTLE_MS, DEFAULT_RES_SETTLE_MS, DEFAULT_CMDLINE_PATH,
        DEFAULT_CMDLINE_KEY, DEFAULT_FALLBACK_RES);
}

int main(int argc, char **argv)
{
    openlog(LOG_TAG, LOG_PID | LOG_CONS, LOG_DAEMON);
    int opt;
    while ((opt = getopt(argc, argv, "d:s:w:i:c:n:m:t:C:K:r:x:FBZQvh")) != -1) {
        switch (opt) {
            case 'd': g_device_path = optarg; break;
            case 's': g_switch_path = optarg; break;
            case 'w': g_res_switch_path = optarg; break;
            case 'i': g_poll_interval_sec = strtod(optarg, NULL); break;
            case 'c': g_cooldown_sec = atoi(optarg); break;
            case 'n': g_confirm_count = atoi(optarg); break;
            case 'm': g_settle_ms = atoi(optarg); break;
            case 't': g_res_settle_ms = atoi(optarg); break;
            case 'C': g_cmdline_path = optarg; break;
            case 'K': g_cmdline_key = optarg; break;
            case 'r': g_fallback_res = atoi(optarg); break;
            case 'x': g_explicit_res = atoi(optarg); break;
            case 'F': g_force_res_config = 0; break;
            case 'B': g_do_bootstrap = 0; break;
            case 'Z': g_res_bump = 0; break;
            case 'v': g_verbose = 1; break;
            case 'h':
            default:
                usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (g_poll_interval_sec < 0.01) g_poll_interval_sec = 0.01;
    if (g_confirm_count < 1) g_confirm_count = 1;
    if (g_settle_ms < 0) g_settle_ms = 0;
    if (g_res_settle_ms < 0) g_res_settle_ms = 0;

    install_signal_handlers();

    LOGI("starting: device=%s switch=%s res_switch=%s fallback_interval=%.2fs cooldown=%ds confirm=%d "
         "settle_ms=%d force_res=%d res_bump=%d fallback_res=%d cmdline=%s key=%s explicit=%d",
         g_device_path, g_switch_path, g_res_switch_path,
         g_poll_interval_sec, g_cooldown_sec, g_confirm_count, g_settle_ms,
         g_force_res_config, g_res_bump, g_fallback_res, g_cmdline_path, g_cmdline_key,
         g_explicit_res);

    int fd = -1;
    int sw_fd = -1, res_fd = -1;

    int consecutive_problem = 0;
    long long cooldown_until_ms = 0;
    long long settle_until_ms = 0;
    int bootstrapped = !g_do_bootstrap;
    int did_first_bringup = 0;
    /* One recovery attempt per HDMI-active epoch. */
    int recovery_attempted_for_active_epoch = 0;
    int last_sw_state = -1;

    while (g_running) {
        /* Open the device and perform the one-time bootstrap BEFORE entering
         * poll().  The old code could sit in a 3-second poll timeout before
         * doing any HDMI bring-up at all. */
        if (fd < 0) {
            fd = open_hdmi_device(g_device_path);
            if (fd < 0) {
                sleep_ms(100);
                continue; /* device node may not exist yet this early in boot */
            }
            LOGI("opened %s", g_device_path);
            bootstrapped = !g_do_bootstrap;
        }

        if (!bootstrapped) {
            long long boot_t = now_ms();
            bring_up_hdmi(fd, "boot", !did_first_bringup);
            did_first_bringup = 1;
            bootstrapped = 1;
            consecutive_problem = 0;
            cooldown_until_ms = boot_t + (long long)(g_cooldown_sec * 1000.0);
            settle_until_ms = boot_t + g_settle_ms;
            continue;
        }

        if (sw_fd < 0) {
            sw_fd = open_switch_fd(g_switch_path);
            rearm_switch_fd(sw_fd);
        }
        if (res_fd < 0) {
            res_fd = open_switch_fd(g_res_switch_path);
            rearm_switch_fd(res_fd);
        }

        struct pollfd pfds[2];
        int nfds = 0;
        int sw_idx = -1, res_idx = -1;

        if (sw_fd >= 0) {
            pfds[nfds].fd = sw_fd;
            pfds[nfds].events = POLLPRI | POLLERR;
            sw_idx = nfds;
            nfds++;
        }
        if (res_fd >= 0) {
            pfds[nfds].fd = res_fd;
            pfds[nfds].events = POLLPRI | POLLERR;
            res_idx = nfds;
            nfds++;
        }

        int timeout_ms = (int)(g_poll_interval_sec * 1000.0);
        if (timeout_ms < 10) timeout_ms = 10;
        int pret = poll(nfds > 0 ? pfds : NULL, nfds, timeout_ms);

        if (!g_running) break;

        if (pret < 0) {
            if (errno != EINTR) {
                LOGD("poll() failed: %s", strerror(errno));
            }
        } else if (pret > 0) {
            if (sw_idx >= 0 && (pfds[sw_idx].revents & (POLLPRI | POLLERR))) {
                LOGD("hdmi switch woke us via sysfs_notify");
                rearm_switch_fd(sw_fd);
            }
            if (res_idx >= 0 && (pfds[res_idx].revents & (POLLPRI | POLLERR))) {
                LOGD("res_hdmi switch woke us via sysfs_notify");
                rearm_switch_fd(res_fd);
            }
        }
        /* pret == 0 is an ordinary periodic fallback wake, same cadence
         * as the old sleep()-based loop. */

        long long t = now_ms();

        if (t < settle_until_ms) {
            LOGD("settling after bring-up (%lldms left)", settle_until_ms - t);
            continue;
        }

        int sw_state  = read_switch_state(g_switch_path);
        int res_state = read_switch_state(g_res_switch_path);
        note_res_switch_value(res_state);

        if (sw_state == 1 && res_state != 0) {
            /* Healthy: driver active and a resolution is configured. */
            consecutive_problem = 0;
            continue;
        }

        int is_problem = 0;
        const char *reason = NULL;

        if (sw_state != last_sw_state) {
            if (sw_state != 1) {
                recovery_attempted_for_active_epoch = 0;
            }
            last_sw_state = sw_state;
            recovery_attempted_for_active_epoch = 0;
            consecutive_problem = 0;
        }

        if (sw_state == 1 && res_state == 0) {
            if (recovery_attempted_for_active_epoch) {
                continue;
            } else {
                is_problem = 1;
                reason = "hdmi switch active but res_hdmi switch is 0 (one recovery attempt)";
            }
        } else if (sw_state != 1) {
            int hw_plugged = query_hw_plug_state(fd);
            if (hw_plugged < 0) {
                LOGD("EDID query failed, will reopen device next cycle");
                close(fd);
                fd = -1;
                continue;
            }
            if (hw_plugged == 1 && !recovery_attempted_for_active_epoch) {
                is_problem = 1;
                reason = "hdmi switch inactive but sink reports plugged in";
            }
        }

        if (!is_problem) {
            consecutive_problem = 0;
            continue;
        }

        consecutive_problem++;
        LOGD("problem detected (%d/%d): %s", consecutive_problem, g_confirm_count, reason);

        if (consecutive_problem >= g_confirm_count) {
            if (t >= cooldown_until_ms) {
                recovery_attempted_for_active_epoch = 1;
                bring_up_hdmi(fd, reason, !did_first_bringup);
                did_first_bringup = 1;
                cooldown_until_ms = t + (long long)(g_cooldown_sec * 1000.0);
                settle_until_ms = t + g_settle_ms;
            } else {
                LOGD("in cooldown (%lldms remaining), skipping bring-up", cooldown_until_ms - t);
            }
            consecutive_problem = 0;
        }
    }

    if (fd >= 0) {
        close(fd);
    }
    if (sw_fd >= 0) {
        close(sw_fd);
    }
    if (res_fd >= 0) {
        close(res_fd);
    }

    LOGI("stopping");
    return 0;
}