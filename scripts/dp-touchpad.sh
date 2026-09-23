#!/bin/bash
# dp-touchpad.sh — 运行时独立开关触摸转发（TOUCHPAD 开关的单件入口）。
#   dp-touchpad.sh on [--device /dev/input/eventN]
#   dp-touchpad.sh off
# TP_NAME 可覆盖自动发现（设备名子串，默认 "touch"）。
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
exec "$ROOT/bin-static/dp-touchpad" "$@"
