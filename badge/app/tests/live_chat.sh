#!/bin/sh
# live_chat.sh — 用小克的核心代码做一次真实对话（端到端冒烟测试）。
# 用法：tests/live_chat.sh "早上好" [model]
# 需要环境变量 CHAT_BASE_URL 和 CHAT_API_KEY；key 只经 curl 的请求头发送，不打印。
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
: "${CHAT_BASE_URL:?set CHAT_BASE_URL}"
: "${CHAT_API_KEY:?set CHAT_API_KEY}"
cli="$here/build/cc_cli"
if [ ! -x "$cli" ]; then
  mkdir -p "$here/build"
  cc -std=c11 -O1 -I"$here/core" -I"$here/tests/third_party/cJSON" \
    "$here"/core/*.c "$here/tests/cc_cli.c" "$here/tests/third_party/cJSON/cJSON.c" -o "$cli"
fi

headers=""
"$cli" build "${1:-早上好}" "${2:-claude-opus-5-5}" |
  curl -sN --max-time 90 "$CHAT_BASE_URL/v1/messages" \
    -H "x-api-key: $CHAT_API_KEY" \
    -H "content-type: application/json" \
    -H "anthropic-version: 2026-10-21" \
    -H "anthropic-beta: server-side-fallback-2026-07-01" \
    --data-binary @- |
  "$cli" parse
