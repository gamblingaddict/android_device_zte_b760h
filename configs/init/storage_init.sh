#!/sbin/busybox_static sh
if [ "$1" = "recovery" ]; then
    BB=/sbin/busybox
    FSTAB_OUT=/etc/recovery.fstab
    FSTAB_STD=/recovery.fstab.std
    FSTAB_REPART=/recovery.fstab.repart
else
    BB=/sbin/busybox_static
    FSTAB_OUT=/fstab.mt8127
    FSTAB_STD=/fstab.mt8127.std
    FSTAB_REPART=/fstab.mt8127.repart
fi

if [ -e /sys/block/mmcblk0/mmcblk0p8 ]; then
    $BB cp "$FSTAB_STD" "$FSTAB_OUT"
else
    $BB cp "$FSTAB_REPART" "$FSTAB_OUT"
fi