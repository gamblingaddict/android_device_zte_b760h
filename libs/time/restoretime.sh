#!/system/bin/sh
FLOOR=631152000 # 1990-01-01

# Only touch the clock if it looks reset
[ "$(date +%s)" -ge $FLOOR ] && exit 0

t=$(getprop persist.sys.lasttime)

# Seed from build date if empty or below the floor
if [ -z "$t" ] || [ "$t" -lt $FLOOR ]; then
  t=$(getprop ro.build.date.utc)
  setprop persist.sys.lasttime "$t"
fi

date @"$t" SET