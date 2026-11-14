# dev.sh — 调试桥的小工具函数（source 后使用）。LOG 指向当前桥日志。
#   k <btn> <ev>      注入按键           shot <名字>   截屏到 /tmp/ccb/<名字>.png
#   waitlog <正则> [秒]  等日志出现
CMD=/tmp/ccb/bridge.cmd
k() { echo "key $1 $2" >> $CMD; sleep "${3:-0.8}"; }
shot() { echo "shot /tmp/ccb/$1.png" >> $CMD; i=0; until grep -q "截屏已保存 /tmp/ccb/$1.png" "$LOG"; do sleep 0.3; i=$((i+1)); [ $i -gt 60 ] && { echo "shot timeout $1"; return 1; }; done; return 0; }
waitlog() { i=0; until grep -q -E "$1" "$LOG"; do sleep 0.5; i=$((i+1)); [ $i -gt $(( ${2:-30} * 2 )) ] && { echo "waitlog timeout: $1"; return 1; }; done; return 0; }
