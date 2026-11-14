#!/bin/sh
# bridge_start.sh <日志名> [端口] — （重新）启动主机桥。需要 PASSPORT_API_BASE / PASSPORT_API_KEY 才能转发对话。
name="$1"; port="${2:-/dev/cu.usbmodem21101}"
here=$(cd "$(dirname "$0")" && pwd)
. "$HOME/esp/esp-idf-v5.5.3/export.sh" >/dev/null 2>&1
mkdir -p /tmp/ccb; pkill -f cc_bridge.py 2>/dev/null; sleep 1
rm -f "/tmp/ccb/$name.log"; : > /tmp/ccb/bridge.cmd
nohup python "$here/cc_bridge.py" --port "$port" --log "/tmp/ccb/$name.log" --cmd /tmp/ccb/bridge.cmd > /tmp/ccb/bridge.out 2>&1 &
