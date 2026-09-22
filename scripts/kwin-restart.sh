#!/bin/bash
# kwin-restart.sh — 常驻接管模式下只重启容器侧 kwinwrap+kwin(不碰 Android 桌面)。
# 以 root 运行。
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
DIR=$ROOT
LOGD=$ROOT/logs
pkill -9 -f "kwinwrap --out" 2>/dev/null
pkill -9 -f "socket=taketest" 2>/dev/null
sleep 1
env KWINWRAP_HIJACK=1 KWINWRAP_FILTER=1 KWINWRAP_SECCOMP=1 \
    KWINWRAP_UID=1000 KWINWRAP_GID=1000 \
    "$DIR/bin/kwinwrap" --out $LOGD/kwinatomic.log -- \
    env -u DISPLAY -u WAYLAND_DISPLAY HOME=/home/xieyizhou \
        KWIN_DRM_DEVICES=/dev/dri/card0 \
        FD_MESA_DEBUG=noubwc \
        KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 \
        XDG_SESSION_ID=bogus \
        XDG_RUNTIME_DIR=/run/user/1000 \
        DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
        kwin_wayland --socket=taketest \
    > $LOGD/kwin.log 2>&1 &
KPID=$!
sleep 5
if kill -0 $KPID 2>/dev/null; then
    echo "KWIN-RESTART OK pid=$KPID"
    grep -iE "input|libinput|touch" $LOGD/kwin.log | tail -8
else
    echo "KWIN-RESTART FAILED; kwin.log tail:"; tail -15 $LOGD/kwin.log
fi
