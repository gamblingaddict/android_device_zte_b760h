#!/system/bin/sh
if [ -e /sys/block/mmcblk0/mmcblk0p8 ]; then
    /system/bin/setprop ro.vold.primary_physical 1
fi