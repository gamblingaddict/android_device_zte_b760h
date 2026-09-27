#!/system/bin/sh

res=$(cat /proc/cmdline | tr ' ' '\n' | grep '^hdmi_res=' | cut -d= -f2)

if [ -n "$res" ]; then
    setprop vendor.hdmi.boot_res "$res"
fi