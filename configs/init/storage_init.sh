#!/sbin/busybox_static sh
if [ -e /sys/block/mmcblk0/mmcblk0p8 ]; then
    /sbin/busybox_static cp /fstab.mt8127.std /fstab.mt8127
else
    /sbin/busybox_static cp /fstab.mt8127.repart /fstab.mt8127
fi