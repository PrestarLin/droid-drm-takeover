#!/bin/bash
# desk-takeover.sh — 全自动无网接力：stop 安卓 → kwin DRM 持屏 + Plasma 桌面 → 容器接管 WiFi。
# 顺序按用户要求：先桌面，再网络。全程不需要我在线（adb 走本机 emulator-5554 通道）。
# 任一关键步失败 → 自动回滚（恢复安卓全家，含 system_suspend 显式拉起，防 Scout 重启）。
ROOT="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
DIR=$ROOT
LOGD=${LOG_DIR:-$(dirname "$ROOT")/logs}
mkdir -p "$LOGD"

# ---- 自脱钩（09-23 黑屏事故教训，同 desk-stop v2）：快捷方式从桌面 konsole 进来时，
#      konsole 是将被本脚本杀掉的 kwin 的客户端；kwin 一死 pty 关闭，前台脚本陪葬，
#      而此时安卓已 stop → 两头全黑。主体必须 setsid 脱离终端。 ----
if [ -z "$DESKSTART_ID" ]; then
    DESKSTART_ID="$$.start"
    export DESKSTART_ID
    setsid nohup "$0" >>"$LOGD/desk-takeover.log" 2>&1 </dev/null &
    CHILD=$!
    tail -n 60 --pid=$CHILD -f "$LOGD/desk-takeover.log" 2>/dev/null
    exit 0
fi
trap '' HUP INT TERM

exec >>"$LOGD/desk-takeover.log" 2>&1
set -x
echo "=== DESK-TAKEOVER START $(date +%F_%T) ==="

WIFI_CONF=/root/desk-wifi.conf
# 仅兜底；正常路径 dhcpcd 拿租约后动态探测 GW/网段（换网不用改这里）
IP=172.16.30.104
PREFIX=22
GW=172.16.30.1

DEV=$(adb devices | awk '$2=="device"{print $1; exit}')
[ -n "$DEV" ] || { echo "NO-ADB-DEVICE"; exit 1; }
run() { adb -s "$DEV" shell "su -c '$1'"; }
kill_linux_stack() {
    # v2: 补全实际 cmdline 模式(v1 的 kwinwrap/socket 模式杀不掉真 kwin)，见 desk-stop.sh 头注
    pkill -9 -f "kwinwrap --out" 2>/dev/null
    pkill -9 -f "socket=taketest" 2>/dev/null
    pkill -9 -f "kwin_wayland_wrapper" 2>/dev/null
    pkill -9 -f "kwin_wayland --" 2>/dev/null
    pkill -9 -f "startplasma-wayland" 2>/dev/null
    pkill -9 -f "plasmashell" 2>/dev/null
    pkill -9 -f "plasma-keyboard" 2>/dev/null
    pkill -9 -f "dmesg-harvester.sh" 2>/dev/null
    pkill -f "$WIFI_CONF" 2>/dev/null
    pkill -x dhcpcd 2>/dev/null
    pkill -f "xdg-desktop-portal" 2>/dev/null
    sleep 1
    fuser -k /dev/dri/card0 2>/dev/null
    rm -f $DIR/takeover.ok
}
rollback() {
    echo "ROLLBACK: $* ($(date +%T))"
    kill_linux_stack
    # system_suspend 被 ctl.stop 后 `start` 拉不起它，新 system_server 会在
    # PowerManagerService.<init> waitForService(android.system.suspend) 卡死
    # → MIUIScout FW_SCOUT_HANG → 自动重启。必须先显式 ctl.start。
    run "echo qoderdbg > /sys/power/wake_unlock; setprop ctl.start system_suspend; setprop ctl.start vendor.qti.hardware.display.composer; start"
    sleep 6
    run "setprop ctl.stop bootanim; sleep 2; setprop ctl.stop bootanim"
    $DIR/bin/setbright 2048 >/dev/null 2>&1
    adb -s "$DEV" shell input keyevent 224 >/dev/null 2>&1
    exit 1
}

# ---- 0) 预检：凭据文件必须在，否则直接不玩 ----
[ -f "$WIFI_CONF" ] || { echo "MISSING $WIFI_CONF"; exit 1; }

# ---- 1) DRM 节点 + udev 合成记录（与 drm-takeover.sh 同源） ----
mkdir -p /dev/dri /dev/input
[ -c /dev/dri/card0 ] || mknod /dev/dri/card0 c 226 0
chmod 666 /dev/dri/card0 2>/dev/null
[ -c /dev/input/event11 ] || mknod /dev/input/event11 c 13 75
chmod 666 /dev/input/event11 2>/dev/null
if [ ! -f /run/udev/data/c226:0 ] || ! grep -q DRIVER /run/udev/data/c226:0; then
    mkdir -p /run/udev/data
    printf 'Q:100\nE:DEVPATH=/devices/platform/soc/ae00000.qcom,mdss_mdp/drm/card0\nE:MAJOR=226\nE:MINOR=0\nE:SUBSYSTEM=drm\nE:DEVTYPE=drm_minor\nE:DEVNAME=dri/card0\nE:DRIVER=vmwgfx\nH:uaccess\nH:seat\n' > /run/udev/data/c226:0
    printf 'Q:101\nE:DEVPATH=/devices/platform/soc/ae00000.qcom,mdss_mdp/drm/renderD128\nE:MAJOR=226\nE:MINOR=128\nE:SUBSYSTEM=drm\nE:DEVTYPE=drm_minor\nE:DEVNAME=dri/renderD128\nE:DRIVER=vmwgfx\nH:uaccess\nH:seat\n' > /run/udev/data/c226:128
    printf 'Q:100\nE:DEVPATH=/devices/virtual/input/input11\nE:MAJOR=13\nE:MINOR=75\nE:SUBSYSTEM=input\nE:DEVNAME=input/event11\nE:ID_INPUT=1\nE:ID_INPUT_TOUCH=1\nE:ID_INPUT_TOUCHSCREEN=1\nE:LIBINPUT_DEVICE_GROUP=11/6/15d9:NVTCapacitiveTouchScreen\nE:LIBINPUT_CALIBRATION_MATRIX=0 1 0 -1 0 1 0 0 1\nH:uaccess\nH:seat\n' > /run/udev/data/c13:75
    chmod -R a+rX /run/udev
fi

# ---- 2) 悬停保护 + 放倒安卓框架（网会掉 ~10-40s，属预期） ----
run "setprop ctl.stop system_suspend"
run "echo qoderdbg > /sys/power/wake_lock"
run "stop"
# stop 不动 class hal！composer HAL 活着就还持有 DRM master（SET_MASTER EBUSY），
# kwin 拿不到屏 → 黑屏（09-21 的坑，drm-takeover 同款处理）
run "setprop ctl.stop vendor.qti.hardware.display.composer"
sleep 5

# ---- 3) kwin 接管显示（SF 已随 stop 死亡，master 天然空闲）→ 先出桌面 ----
kill_linux_stack
rm -f $DIR/takeover.ok
env KWINWRAP_HIJACK=1 KWINWRAP_FILTER=1 KWINWRAP_SECCOMP=1 \
    KWINWRAP_UID=1000 KWINWRAP_GID=1000 KWINWRAP_BRIGHTNESS=2048 \
    $DIR/bin/kwinwrap --out $LOGD/kwinatomic.log -- \
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
sleep 6
kill -0 $KPID 2>/dev/null || rollback "kwin died (see kwin.log)"
runuser -u xieyizhou -- env -u DISPLAY WAYLAND_DISPLAY=taketest \
    HOME=/home/xieyizhou XDG_RUNTIME_DIR=/run/user/1000 \
    QT_QPA_PLATFORM=wayland \
    timeout 5 wayland-info > $LOGD/wayland-info.log 2>&1
[ $? = 0 ] || rollback "wayland-info self-check failed"
# ---- 3b) 上屏取证 + 强制点亮：stop 时 system_server 死前会走关机流程把屏灭掉，
#      kwin 新 commit 不一定把 connector DPMS 拉回 On → 黑屏。主动写 dpms=0。 ----
$DIR/bin/crtcstate > $LOGD/crtcstate-desk.log 2>&1
$DIR/bin/connprops > $LOGD/connprops-desk.log 2>&1
$DIR/bin/setprop 0 DPMS
$DIR/bin/setbright 2048 > /dev/null 2>&1
sleep 2
$DIR/bin/crtcstate > $LOGD/crtcstate-desk2.log 2>&1

# ---- 4) Plasma 桌面 ----
# /etc/environment 的 QT_IM_MODULE=fcitx5 会把 Qt 应用的 text-input 抢去 fcitx，
# kwin 收不到聚焦事件 → plasma-keyboard 永远不弹（Chrome 自带协议所以能弹）。
# 一律清掉，让 Qt 回退到 compositor 内置 text-input。
nohup runuser -u xieyizhou -- env -u DISPLAY -u QT_IM_MODULE -u GTK_IM_MODULE \
    -u SDL_IM_MODULE -u GLFW_IM_MODULE -u XMODIFIERS \
    WAYLAND_DISPLAY=taketest \
    HOME=/home/xieyizhou XDG_RUNTIME_DIR=/run/user/1000 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    QT_QPA_PLATFORM=wayland \
    /usr/bin/plasmashell --replace > $LOGD/plasma.log 2>&1 &
# 任务栏点击启动应用走 xdg-desktop-portal；不带 KDE 环境起来的话只有 gtk 后端
nohup runuser -u xieyizhou -- env -u DISPLAY -u QT_IM_MODULE -u GTK_IM_MODULE \
    -u SDL_IM_MODULE -u GLFW_IM_MODULE -u XMODIFIERS \
    WAYLAND_DISPLAY=taketest XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland \
    HOME=/home/xieyizhou XDG_RUNTIME_DIR=/run/user/1000 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    QT_QPA_PLATFORM=wayland \
    /usr/libexec/xdg-desktop-portal > $LOGD/portal.log 2>&1 &
touch $DIR/takeover.ok
$DIR/bin/setbright 2048 > /dev/null 2>&1
echo "DESKTOP-UP $(date +%T) kwin pid $KPID"

# ---- 5) 容器接管 WiFi（桌面已在屏上，网络是第二条腿） ----
pkill -f "$WIFI_CONF" 2>/dev/null
pkill -x dhcpcd 2>/dev/null
sleep 1
ip link set wlan0 down; sleep 1; ip link set wlan0 up
mkdir -p /run/wpa-takeover
/usr/sbin/wpa_supplicant -i wlan0 -c "$WIFI_CONF" -P /run/wpa-takeover.pid > /run/wpa-takeover.log 2>&1 &
OK=0
for i in $(seq 1 30); do
    sleep 1
    wpa_cli -p /run/wpa-takeover -i wlan0 status 2>/dev/null | grep -q "wpa_state=COMPLETED" && { OK=1; break; }
done
if [ "$OK" != 1 ]; then
    echo "--- wpa diagnosis ---"
    wpa_cli -p /run/wpa-takeover -i wlan0 status
    tail -n 25 /run/wpa-takeover.log
    rollback "wifi assoc timeout (30s)"
fi
echo "WIFI-ASSOC OK $(date +%T), now DHCP"
# 安卓 netd 留的旧地址是 noprefixroute（main 表没直连路由，默认路由会被拒
# "Nexthop has invalid gateway"）→ 先冲干净，让 dhcpcd 完整接管；
# 兜底路由必须带 onlink 跳过网关可达性检查。
ip addr flush dev wlan0
dhcpcd -k wlan0 2>/dev/null
pkill -9 -x dhcpcd 2>/dev/null; sleep 1
dhcpcd -4 -w -t 20 wlan0 || echo "dhcpcd exited rc=$?"
sleep 1
ip -4 route show default dev wlan0 | grep -q default || ip -4 route replace default via $GW dev wlan0 onlink
# 动态取租约网段+网关（换网免改脚本）
LEASED=$(ip -4 -br addr show wlan0 | awk '{print $3}' | head -1)
DYN_GW=$(ip -4 route show default dev wlan0 | awk '{print $3; exit}')
if [ -n "$LEASED" ]; then
    NETV=$(python3 -c "import ipaddress,sys; a=ipaddress.IPv4Interface(sys.argv[1]); print(a.network)" "$LEASED" 2>/dev/null)
fi
[ -n "$DYN_GW" ] && GW="$DYN_GW"
# 安卓删掉了 "lookup main" 规则，本机所有包（含 root ping）都走
# "fwmark 0/0xffff lookup 1015"；link down/up 又清空了 1015 → main 配再好也不通。
ip -4 route replace ${NETV:-172.16.30.0/22} dev wlan0 table 1015
ip -4 route replace default via $GW dev wlan0 table 1015 onlink
ip -4 addr show dev wlan0
ip -4 route show
rm -f /etc/resolv.conf
printf 'nameserver 223.5.5.5\nnameserver 114.114.114.114\n' > /etc/resolv.conf
NET=0
for i in 1 2 3; do
    ping -c 2 -W 2 223.5.5.5 >/dev/null 2>&1 && { NET=1; break; }
    sleep 2
done
if [ "$NET" != 1 ]; then
    echo "--- egress diagnosis ---"
    ip route show table all | head -n 30
    cat /etc/resolv.conf
    rollback "no egress after dhcp"
fi
echo "NET-TAKEOVER OK $(date +%T)"

# ---- 6) 收尾：取证收割机 + 状态 ----
DEV=$DEV nohup bash $DIR/scripts/dmesg-harvester.sh > /dev/null 2>&1 &
adb -s "$DEV" shell "su -c 'free -m | head -2'"
echo "=== DESK-TAKEOVER DONE $(date +%T): kwin pid $KPID, plasma up, net via container wpa+dhcpcd ==="
