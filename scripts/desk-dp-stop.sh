#!/bin/bash
# desk-dp-stop.sh — 结束 DP alt mode 会话，顺序按设计：
#   1) helper revoke  （内核发 hotplug，SF 立刻重新看见 DP 口——先还权）
#   2) 杀 kwin/Plasma 栈（kwin 一关，lessee fd 落地销毁，双保险）
#   3) 杀 helper      （关掉 socket/租约 fd；若 kwin 已死这里就是 lease destroy）
#   4) 开关归位       （dp-touchpad off / dp-screenctl off：解 grab、恢复背光、wake_unlock）
#
# 绝不做的事：不停安卓、不 fuser card0（SF 还活着，kill 它=自己砸场子）。
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
DIR=$ROOT
LOGD=${LOG_DIR:-$(dirname "$ROOT")/logs}
mkdir -p "$LOGD"
LOG=$LOGD/desk-dp-stop.log

if [ -z "$DPSTOP_ID" ]; then
    DPSTOP_ID="$$.start"
    export DPSTOP_ID
    setsid nohup "$0" >>"$LOG" 2>&1 </dev/null &
    CHILD=$!
    tail -n 40 --pid=$CHILD -f "$LOG" 2>/dev/null
    exit 0
fi
trap '' HUP INT TERM
set -x
echo "=== DESK-DP-STOP START $(date +%F_%T) ==="

HELPER=$DIR/bin-static/dp-lease-helper

# ---- 1) 先还权：revoke → 内核 hotplug → SF 重查 → DP 回到安卓 ----
if [ -x "$HELPER" ]; then
    timeout 5 "$HELPER" revoke || echo "revoke via socket failed (helper dead?)"
fi
sleep 1

# ---- 2) 杀 DP 桌面栈（模式限定 dpdesk，别误伤常驻 taketest/系统会话） ----
kill_dp_stack() {
    pkill -9 -f "socket=dpdesk" 2>/dev/null
    pkill -9 -f "kwin_wayland --socket=dpdesk" 2>/dev/null
    pkill -9 -f "LD_PRELOAD=.*kwin-drm-shim" 2>/dev/null
    pkill -9 -f "plasmashell" 2>/dev/null
    pkill -9 -f "kactivitymanagerd" 2>/dev/null
    pkill -9 -f "xdg-desktop-portal" 2>/dev/null
    pkill -9 -f "dpdesk" 2>/dev/null
}
for i in 1 2 3; do
    kill_dp_stack
    sleep 1
    ALIVE=$(pgrep -f "dpdesk|kwin-drm-shim" | grep -v "^$$\$")
    [ -z "$ALIVE" ] && break
    kill -9 $ALIVE 2>/dev/null
done

# ---- 3) helper 收摊（若 revoke 没走 socket，这里 TERM 触发 destroy+hotplug） ----
pkill -f "dp-lease-helper daemon" 2>/dev/null
sleep 1
rm -f /data/local/tmp/drm-lease.sock /run/drm-lease.sock /tmp/drm-lease.sock 2>/dev/null

# ---- 4) 开关归位 ----
[ -x "$DIR/bin-static/dp-touchpad" ] && timeout 5 "$DIR/bin-static/dp-touchpad" off || true
[ -x "$DIR/bin-static/dp-screenctl" ] && timeout 5 "$DIR/bin-static/dp-screenctl" off || true

rm -f "$DIR/dp-takeover.ok"
echo "=== DESK-DP-STOP DONE $(date +%T)：安卓未被触碰，DP 口已热插拔事件还给 SF ==="
