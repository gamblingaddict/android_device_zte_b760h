#!/system/bin/sh
FLOOR=631152000 # 1990-01-01
while true; do
  now=$(date +%s)
  [ "$now" -ge $FLOOR ] && setprop persist.sys.lasttime "$now"
  sleep 600
done