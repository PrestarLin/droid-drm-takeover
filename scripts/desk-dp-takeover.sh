#!/bin/bash
# desk-dp-takeover.sh — DP alt mode：容器画面经 Type-C DP 输出，安卓内屏照常归 SF。
#
# 与 desk-takeover.sh 的本质区别：**不停安卓**、不碰 master。容器侧 root 的
# dp-lease-helper 先铸一枚 DRM_MODE_LEASE_EXCL 独占租约（DP connector + 空闲
# CRTC + 其 planes），内核三个补丁（kernel-patches/）负责让 SF 视角里 DP 口
# “不存在/未插入”，kwin 再通过 LD_PRELOAD 的 kwin-drm-shim.so 拿到租约 fd
# 当自己的 card0 用——两边互不抢、互不可见。
#
# 开关（环境变量）：
#   TOUCHPAD=on|off    (默认 off) 触摸屏 EVIOCGRAB → uinput 克隆给容器当触屏
#   SCREEN_OFF=display|lock
#        display (默认): wake_lock + 背光置 0 + 吞掉 KEY_POWER（音量等经
#                        uinput 转发）——内屏“黑掉”但绝不休眠（休眠会断 DP）。
#        lock:          只 wake_lock，锁屏/熄屏走安卓原生逻辑。
#
# 退出：scripts/desk-dp-stop.sh（顺序 = revoke → 杀 kwin → 杀 helper → 开关归位）。
ROOT="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
DIR=$ROOT
LINUX_USER=${LINUX_USER:-xieyizhou}
LINUX_HOME=${LINUX_HOME:-/home/$LINUX_USER}
LOGD=${LOG_DIR:-$(dirname "$ROOT")/logs}
mkdir -p "$LOGD"

TOUCHPAD=${TOUCHPAD:-off}
SCREEN_OFF=${SCREEN_OFF:-display}
case "$SCREEN_OFF" in display|lock) ;; *) echo "bad SCREEN_OFF=$SCREEN_OFF"; exit 2;; esac

# 自脱钩（同 desk-takeover：konsole 是 kwin 的客户端，kwin 一死 pty 连带脚本）
if [ -z "$DPSTART_ID" ]; then
    DPSTART_ID="$$.start"
    export DPSTART_ID
    setsid nohup "$0" >>"$LOGD/desk-dp-takeover.log" 2>&1 </dev/null &
    CHILD=$!
    tail -n 60 --pid=$CHILD -f "$LOGD/desk-dp-takeover.log" 2>/dev/null
    exit 0
fi
trap '' HUP INT TERM

exec >>"$LOGD/desk-dp-takeover.log" 2>&1
set -x
echo "=== DESK-DP-TAKEOVER START $(date +%F_%T) TOUCHPAD=$TOUCHPAD SCREEN_OFF=$SCREEN_OFF ==="

HELPER=$DIR/bin-static/dp-lease-helper
SHIM=$DIR/bin/kwin-drm-shim.so
[ -x "$HELPER" ] || { echo "NO-HELPER $HELPER (run make)"; exit 1; }
[ -f "$SHIM" ] || { echo "NO-SHIM $SHIM (run make)"; exit 1; }
[ -c /dev/dri/card0 ] || { echo "NO-CARD0"; exit 1; }
# 内核补丁检查：旧内核没有 DRM_MODE_LEASE_EXCL —— helper 铸租时会 EINVAL
if ! grep -q DRM_MODE_LEASE_EXCL /proc/kallsyms 2>/dev/null; then
    echo "WARN: kernel patches not confirmed via kallsyms (flag is uapi, not a symbol); continuing"
fi

SOCK=${DP_LEASE_SOCK:-/data/local/tmp/drm-lease.sock}
export DP_LEASE_SOCK=$SOCK
SOCKDIR=$(dirname "$SOCK")
mkdir -p "$SOCKDIR" 2>/dev/null

# ---- 0) 清掉上一轮残留（revoke 兜底；helper flock 防双开） ----
$HELPER revoke >/dev/null 2>&1 || true
pkill -f "dp-lease-helper daemon" 2>/dev/null || true
sleep 1

# ---- 1) 铸独占租约 + 起 socket 服务 ----
rm -f "$SOCK"
nohup "$HELPER" daemon --card /dev/dri/card0 \
    >"$LOGD/dp-lease-helper.log" 2>&1 &
HPID=$!
for i in $(seq 1 40); do
    [ -S "$SOCK" ] && break
    kill -0 $HPID 2>/dev/null || break
    sleep 0.25
done
if ! [ -S "$SOCK" ]; then
    echo "HELPER-FAILED; log:"
    tail -20 "$LOGD/dp-lease-helper.log"
    # 铸租失败多半 = 内核没打补丁（旧内核 CREATE_LEASE 挂 DRM_MASTER，root 非 master → EACCES/EINVAL）
    exit 1
fi
echo "HELPER-UP pid=$HPID sock=$SOCK"
"$HELPER" status || true

# ---- 2) SCREEN_OFF 策略（display/lock 都必须 wake_lock，否则 suspend 断 DP） ----
case "$SCREEN_OFF" in
display) nohup "$DIR/bin-static/dp-screenctl" display \
             >"$LOGD/dp-screenctl.log" 2>&1 & ;;
lock)    "$DIR/bin-static/dp-screenctl" lock \
             >>"$LOGD/dp-screenctl.log" 2>&1 ;;
esac
sleep 0.5

# ---- 3) TOUCHPAD（默认 off：内屏触摸保持安卓原样） ----
if [ "$TOUCHPAD" = "on" ]; then
    nohup "$DIR/bin-static/dp-touchpad" on \
        >"$LOGD/dp-touchpad.log" 2>&1 &
    sleep 1
fi

# ---- 4) kwin 以租约 fd 当 card0（shim 拦 open；绝不带 KWINWRAP_HIJACK——
#         那套是“停安卓抢 master”路线，这里 SF 还活着，抢=EBUSY 打架） ----
pkill -9 -f "socket=dpdesk" 2>/dev/null || true
sleep 1
rm -f "$DIR/dp-takeover.ok"
env -u DISPLAY -u WAYLAND_DISPLAY \
    LD_PRELOAD="$SHIM" DP_LEASE_SOCK="$SOCK" \
    HOME="$LINUX_HOME" \
    KWIN_DRM_DEVICES=/dev/dri/card0 \
    FD_MESA_DEBUG=noubwc \
    KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 \
    XDG_SESSION_ID=bogus \
    XDG_RUNTIME_DIR=/run/user/1000 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    kwin_wayland --socket=dpdesk \
    >"$LOGD/dp-kwin.log" 2>&1 &
KPID=$!
sleep 6
if ! kill -0 $KPID 2>/dev/null; then
    echo "KWIN-DIED; dp-kwin.log tail:"
    tail -20 "$LOGD/dp-kwin.log"
    exit 1
fi
runuser -u "$LINUX_USER" -- env -u DISPLAY WAYLAND_DISPLAY=dpdesk \
    HOME="$LINUX_HOME" XDG_RUNTIME_DIR=/run/user/1000 \
    QT_QPA_PLATFORM=wayland \
    timeout 5 wayland-info >"$LOGD/dp-wayland-info.log" 2>&1
[ $? = 0 ] || { echo "wayland-info self-check failed"; exit 1; }
echo "KWIN-DP-UP pid=$KPID"

# ---- 5) Plasma 桌面（同 desk-takeover，socket 换 dpdesk，不碰安卓） ----
nohup runuser -u "$LINUX_USER" -- env -u DISPLAY -u QT_IM_MODULE -u GTK_IM_MODULE -u XMODIFIERS \
    QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY=dpdesk \
    HOME="$LINUX_HOME" XDG_RUNTIME_DIR=/run/user/1000 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    /usr/lib/aarch64-linux-gnu/libexec/kactivitymanagerd >"$LOGD/dp-kactivitymanagerd.log" 2>&1 &
for i in $(seq 1 10); do
    runuser -u "$LINUX_USER" -- env DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
        gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.ListNames 2>/dev/null | grep -q org.kde.ActivityManager && break
    sleep 1
done
nohup runuser -u "$LINUX_USER" -- env -u DISPLAY -u QT_IM_MODULE -u GTK_IM_MODULE \
    -u SDL_IM_MODULE -u GLFW_IM_MODULE -u XMODIFIERS \
    WAYLAND_DISPLAY=dpdesk \
    HOME="$LINUX_HOME" XDG_RUNTIME_DIR=/run/user/1000 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    QT_QPA_PLATFORM=wayland \
    /usr/bin/plasmashell --replace >"$LOGD/dp-plasma.log" 2>&1 &
nohup runuser -u "$LINUX_USER" -- env -u DISPLAY -u QT_IM_MODULE -u GTK_IM_MODULE \
    -u SDL_IM_MODULE -u GLFW_IM_MODULE -u XMODIFIERS \
    WAYLAND_DISPLAY=dpdesk XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland \
    HOME="$LINUX_HOME" XDG_RUNTIME_DIR=/run/user/1000 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    QT_QPA_PLATFORM=wayland \
    /usr/libexec/xdg-desktop-portal >"$LOGD/dp-portal.log" 2>&1 &

touch "$DIR/dp-takeover.ok"
echo "=== DESK-DP-TAKEOVER DONE $(date +%T): helper=$HPID kwin=$KPID sock=$SOCK ==="
echo "插上 Type-C DP 线即出画面；SF 视角 DP 口被独占租约隐藏（getconnector 恒 disconnected）。"
