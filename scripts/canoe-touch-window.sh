#!/system/bin/sh
# OnePlus 15 (canoe) touch window: stop Android display stack -> touchdraw
# -> unconditional restore. Runs detached from adb (setsid/nohup).
LOG=/data/local/tmp/probes/touch.log
exec >>"$LOG" 2>&1
echo "=== START $(date) ==="
cd /data/local/tmp/probes/s || exit 1
BL=$(cat /sys/class/backlight/panel0-backlight/brightness)
# anti-blank (project lesson): SF dead => PMS => system_suspend blanks panel
setprop ctl.stop system_suspend
echo qoderdbg > /sys/power/wake_lock
setprop ctl.stop surfaceflinger
setprop ctl.stop vendor.qti.hardware.display.composer
sleep 2
timeout 75 ./touchdraw /dev/dri/card0 55
echo "TOUCHDRAW_RC=$?"
echo "=== RESTORE ==="
setprop ctl.start vendor.qti.hardware.display.composer
sleep 2
setprop ctl.start surfaceflinger
sleep 4
setprop ctl.stop bootanim
echo qoderdbg > /sys/power/wake_unlock
setprop ctl.start system_suspend
echo "$BL" > /sys/class/backlight/panel0-backlight/brightness
sleep 2
echo "VERIFY sf=$(getprop init.svc.surfaceflinger) composer=$(getprop init.svc.vendor.qti.hardware.display.composer) zygote=$(getprop init.svc.zygote)"
echo "=== END $(date) ==="
