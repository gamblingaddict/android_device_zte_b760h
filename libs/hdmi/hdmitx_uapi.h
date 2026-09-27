#ifndef _HDMITX_UAPI_H
#define _HDMITX_UAPI_H

#include <sys/ioctl.h>

/*
 * Minimal userspace-safe subset of the MTK hdmitx kernel driver UAPI,
 * extracted from hdmitx.h / hdmitable.h for MTK_INTERNAL_HDMI_SUPPORT
 * targets (kernel 3.4 / Android 7, MT65xx-MT81xx family SoCs).
 *
 * Only the pieces the watchdog daemon actually needs are kept here so
 * it can be built without pulling in the full kernel header tree.
 */

#define HDMI_IOW(num, dtype)     _IOW('H', num, dtype)
#define HDMI_IOR(num, dtype)     _IOR('H', num, dtype)
#define HDMI_IOWR(num, dtype)    _IOWR('H', num, dtype)
#define HDMI_IO(num)             _IO('H', num)

/* Enables/disables the whole audio+video pipeline (arg: 1 = on, 0 = off).
 * Nothing else in the driver calls this on its own - some external
 * component (normally the display HAL/service) is expected to. Until
 * it's called, p->is_enabled stays false, hdmi_power_on() never runs,
 * the chip never leaves OFF, and the HPD-driven switch never updates -
 * regardless of whether a cable is actually plugged in. */
#define MTK_HDMI_AUDIO_VIDEO_ENABLE   HDMI_IO(1)

/* Separate audio mute/unmute toggle. Requires AUDIO_VIDEO_ENABLE(1) to
 * have already been issued (RETIF(!p->is_enabled, 0) gates it). */
#define MTK_HDMI_AUDIO_ENABLE         HDMI_IO(2)

/* Powers the chip/DPI clocks on or off without touching is_enabled (arg: 1/0) */
#define MTK_HDMI_POWER_ENABLE         HDMI_IOW(12, int)

/* Software force-disable / re-enable pair (only undoes a prior FORCE_ON) */
#define MTK_HDMI_FORCE_FULLSCREEN_ON  HDMI_IOWR(8, int)
#define MTK_HDMI_FORCE_FULLSCREEN_OFF HDMI_IOWR(9, int)

/* Forces the video pipeline to (re)configure to the given resolution.
 * arg is one of the HDMI_VIDEO_RESOLUTION enum values from hdmitable.h.
 * On MTK_HDMI_MAIN_PATH builds this bypasses the "state must be ON"
 * gate other ioctls have, which is why it can succeed even before the
 * hotplug/HPD state machine has caught up. Confirmed against real
 * device logs: enabling audio+video alone is not sufficient to
 * actually produce a picture on this driver variant - this ioctl is
 * what triggers tmds on / vid+aud unmute / RDMA+mutex bring-up. The
 * driver treats it as a no-op if already at the requested resolution
 * (see the "hdmi_reschange == arg" early-out in hdmitx.c), so it's
 * safe to call unconditionally. */
#define MTK_HDMI_VIDEO_CONFIG   HDMI_IOWR(6, int)

/* A handful of HDMI_VIDEO_RESOLUTION values from hdmitable.h, enough to
 * pick a sane bootstrap default.  */
#define HDMI_VIDEO_RES_720x480p_60Hz    0
#define HDMI_VIDEO_RES_720x576p_50Hz    1
#define HDMI_VIDEO_RES_1280x720p_60Hz   2
#define HDMI_VIDEO_RES_1280x720p_50Hz   3
#define HDMI_VIDEO_RES_1920x1080i_60Hz  4
#define HDMI_VIDEO_RES_1920x1080i_50Hz  5
#define HDMI_VIDEO_RES_1920x1080p_30Hz  6
#define HDMI_VIDEO_RES_1920x1080p_25Hz  7
#define HDMI_VIDEO_RES_1920x1080p_24Hz  8
#define HDMI_VIDEO_RES_1920x1080p_23Hz  9
#define HDMI_VIDEO_RES_1920x1080p_29Hz  10
#define HDMI_VIDEO_RES_1920x1080p_60Hz  11
#define HDMI_VIDEO_RES_1920x1080p_50Hz  12

/* Blocking factory self-test: loops up to ~3s polling IS_HDMI_ON().
 * NOT useful for detecting "cable plugged but driver thinks it's off",
 * because it checks the driver's own (stuck) software state - kept
 * here for completeness/logging only. */
#define MTK_HDMI_FACTORY_GET_STATUS   HDMI_IOWR(31, int)

/* Reads EDID info straight from the sink chip - ui1_sink_is_plug_in
 * reflects real hardware plug state and is what this daemon relies on. */
#define MTK_HDMI_GET_EDID             HDMI_IOWR(61, HDMI_EDID_T)

typedef struct _HDMI_EDID_T
{
    unsigned int ui4_ntsc_resolution;
    unsigned int ui4_pal_resolution;
    unsigned int ui4_sink_native_ntsc_resolution;
    unsigned int ui4_sink_native_pal_resolution;
    unsigned int ui4_sink_cea_ntsc_resolution;
    unsigned int ui4_sink_cea_pal_resolution;
    unsigned int ui4_sink_dtd_ntsc_resolution;
    unsigned int ui4_sink_dtd_pal_resolution;
    unsigned int ui4_sink_1st_dtd_ntsc_resolution;
    unsigned int ui4_sink_1st_dtd_pal_resolution;
    unsigned short ui2_sink_colorimetry;
    unsigned char  ui1_sink_rgb_color_bit;
    unsigned char  ui1_sink_ycbcr_color_bit;
    unsigned short ui2_sink_aud_dec;
    unsigned char  ui1_sink_is_plug_in;   /* <-- the field we actually use */
    unsigned int   ui4_hdmi_pcm_ch_type;
    unsigned int   ui4_hdmi_pcm_ch3ch4ch5ch7_type;
    unsigned int   ui4_dac_pcm_ch_type;
    unsigned char  ui1_sink_i_latency_present;
    unsigned char  ui1_sink_p_audio_latency;
    unsigned char  ui1_sink_p_video_latency;
    unsigned char  ui1_sink_i_audio_latency;
    unsigned char  ui1_sink_i_video_latency;
    unsigned char  ui1ExtEdid_Revision;
    unsigned char  ui1Edid_Version;
    unsigned char  ui1Edid_Revision;
    unsigned char  ui1_Display_Horizontal_Size;
    unsigned char  ui1_Display_Vertical_Size;
    unsigned int   ui4_ID_Serial_Number;
    unsigned int   ui4_sink_cea_3D_resolution;
    unsigned char  ui1_sink_support_ai;
    unsigned short ui2_sink_cec_address;
    unsigned short ui1_sink_max_tmds_clock;
    unsigned short ui2_sink_3D_structure;
    unsigned int   ui4_sink_cea_FP_SUP_3D_resolution;
    unsigned int   ui4_sink_cea_TOB_SUP_3D_resolution;
    unsigned int   ui4_sink_cea_SBS_SUP_3D_resolution;
    unsigned short ui2_sink_ID_manufacturer_name;
    unsigned short ui2_sink_ID_product_code;
    unsigned int   ui4_sink_ID_serial_number;
    unsigned char  ui1_sink_week_of_manufacture;
    unsigned char  ui1_sink_year_of_manufacture;
} HDMI_EDID_T;

#define HDMI_DRV_NODE "/dev/hdmitx"
#define HDMI_SWITCH_STATE_PATH "/sys/class/switch/hdmi/state"
#define HDMI_RES_SWITCH_STATE_PATH "/sys/class/switch/res_hdmi/state"

#endif /* _HDMITX_UAPI_H */