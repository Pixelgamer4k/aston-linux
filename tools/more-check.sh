#!/usr/bin/bash
echo "== CPU =="
/usr/bin/grep -c ^processor /proc/cpuinfo
/usr/bin/grep -m1 "CPU part" /proc/cpuinfo
echo "== MEMORY =="
/usr/bin/grep MemTotal /proc/meminfo
echo "== DISPLAY =="
/usr/bin/cat /sys/class/drm/card0-DSI-1/status
/usr/bin/cat /sys/class/drm/card0-DSI-1/modes
/usr/bin/cat /sys/class/drm/card0-DSI-1/enabled
echo "== GPU =="
/usr/bin/ls /sys/class/kgsl 2>/dev/null || true
/usr/bin/ls /dev/dri 2>/dev/null || true
echo "== BATTERY =="
/usr/bin/cat /sys/class/power_supply/battery/capacity
/usr/bin/cat /sys/class/power_supply/battery/status
/usr/bin/cat /sys/class/power_supply/battery/temp
echo "== AUDIO =="
/usr/bin/cat /proc/asound/cards
echo "== CAMERA NODES =="
/usr/bin/ls /dev/video* 2>/dev/null || echo none
echo "== BLUETOOTH =="
/usr/bin/ls /sys/class/bluetooth 2>/dev/null || echo none
echo "== SENSORS =="
/usr/bin/ls /sys/bus/iio/devices 2>/dev/null || echo none
echo "== WLAN =="
/usr/sbin/ip -4 addr show wlan0
echo "== PING =="
/usr/bin/ping -c 2 -W 3 1.1.1.1 || echo ping_missing_or_failed
echo "== TOUCH SAMPLE =="
/usr/bin/timeout 2 /usr/bin/dd if=/dev/input/event6 of=/tmp/touch.sample bs=24 count=4 2>/tmp/touch.err || true
/usr/bin/wc -c /tmp/touch.sample 2>/dev/null || echo no_touch_bytes
echo "== BLOCK READ =="
/usr/bin/dd if=/dev/block/by-name/userdata of=/tmp/sb.sample bs=4096 count=1 2>/tmp/dd.err || true
/usr/bin/wc -c /tmp/sb.sample
