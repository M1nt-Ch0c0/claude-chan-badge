#!/usr/bin/env python3
"""假徽章：连到 VoiceDesk（默认 127.0.0.1:19732），按实时速度送一段语音，记录回来的阶段、字幕和声音。

用法：
  VOICEDESK_PORT=19732 .build/debug/VoiceDesk &      # 另起一个实例，不打扰真徽章
  python3 fake_badge.py "小克，帮我看看下载文件夹里最大的两个文件" --seconds 70

检查点：
- 字幕帧（TEXT 0x12）是逐步变长的，小克那句的出现速度大致跟着播放进度走
- 阶段顺序 1 在听 → 2 在想 → 3 在说 → 0
- 收到的 PCM 帧速率接近每秒 50 帧（按实时速度送），而不是一下子灌满
"""
import argparse
import io
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.request
import wave

MAGIC = 0xC3A1
HELLO, HELLO_ACK, PCM, CTRL, TEXT, SUBIMG = 1, 2, 0x10, 0x11, 0x12, 0x13
FRAME = 640


def load_auth():
    """和 VoiceDesk 同样的顺序找地址和密钥：环境变量 → ~/.config/voicedesk/config.json。"""
    cfg = {}
    path = os.path.expanduser("~/.config/voicedesk/config.json")
    if os.path.exists(path):
        cfg = json.load(open(path))
    key = os.environ.get("VOICEDESK_API_KEY") or cfg.get("api_key")
    base = os.environ.get("VOICEDESK_BASE_URL") or cfg.get("base_url") or "https://api.openai.com"
    if not key:
        sys.exit("需要 VOICEDESK_API_KEY（用来把测试句子转成语音）")
    return base.rstrip("/"), key


def tts16k(text):
    base, key = load_auth()
    req = urllib.request.Request(base + "/v1/audio/speech", method="POST",
        data=json.dumps({"model": "gpt-4o-mini-tts", "input": text, "voice": "alloy", "response_format": "wav"}).encode(),
        headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"})
    raw = urllib.request.urlopen(req, timeout=60).read()
    w = wave.open(io.BytesIO(raw))
    rate, pcm = w.getframerate(), w.readframes(w.getnframes())
    samples = struct.unpack("<%dh" % (len(pcm) // 2), pcm)
    step = rate / 16000
    out = [samples[int(i * step)] for i in range(int(len(samples) / step))]
    return struct.pack("<%dh" % len(out), *out)


def save_png(path, w, h, a4):
    import zlib
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            b = a4[y * (w // 2) + x // 2]
            v = ((b >> 4) if x % 2 == 0 else (b & 15)) * 17
            raw += bytes((255 - v, 255 - v, 255 - v))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def frame(kind, seq, payload=b""):
    return struct.pack("<HBBIH", MAGIC, kind, 0, seq, len(payload)) + payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text")
    ap.add_argument("--port", type=int, default=19732)
    ap.add_argument("--seconds", type=float, default=40)
    ap.add_argument("--bitmap", metavar="DIR", help="声明支持字幕位图，把每次提交的位图存成 PNG")
    args = ap.parse_args()

    speech = tts16k(args.text)
    print(f"语音 {len(speech) / 32000:.1f} 秒", flush=True)
    s = socket.create_connection(("127.0.0.1", args.port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    t0 = time.time()
    seq = [1]

    def send(kind, payload=b""):
        s.sendall(frame(kind, seq[0], payload))
        seq[0] += 1

    send(HELLO, struct.pack("<IBB", 16000, 1, 1 if args.bitmap else 0))
    img = {"w": 0, "h": 0, "buf": bytearray(), "n": 0, "bytes": 0}
    if args.bitmap:
        os.makedirs(args.bitmap, exist_ok=True)
    stats = {"pcm": 0, "pcm_t": []}
    last_text = [None]
    stop = [False]

    def reader():
        buf = b""
        while not stop[0]:
            try:
                data = s.recv(65536)
            except OSError:
                break
            if not data:
                break
            buf += data
            while len(buf) >= 10:
                magic, kind, _flags, _seq, n = struct.unpack("<HBBIH", buf[:10])
                if magic != MAGIC:
                    buf = buf[1:]
                    continue
                if len(buf) < 10 + n:
                    break
                payload, buf = buf[10:10 + n], buf[10 + n:]
                t = time.time() - t0
                if kind == PCM:
                    stats["pcm"] += 1
                    stats["pcm_t"].append(t)
                elif kind == CTRL:
                    print(f"{t:6.2f} CTRL {payload.hex()}", flush=True)
                elif kind == TEXT:
                    role = payload[0]
                    text = payload[1:].decode("utf-8", "replace")
                    print(f"{t:6.2f} TEXT[{role}] pcm={stats['pcm']:4d} {text!r}", flush=True)
                    last_text[0] = (role, text)
                elif kind == SUBIMG:
                    role, flags, w, h, y0, rows = payload[:6]
                    img["bytes"] += len(payload) + 10
                    if (w, h) != (img["w"], img["h"]):
                        img.update(w=w, h=h, buf=bytearray(w // 2 * h))
                    st = w // 2
                    img["buf"][y0 * st:(y0 + rows) * st] = payload[6:6 + rows * st]
                    if flags & 1:
                        img["n"] += 1
                        print(f"{t:6.2f} SUBIMG[{role}] {w}x{h} 改了 {rows} 行 起始 {y0}", flush=True)
                        if args.bitmap and w and h:
                            save_png(os.path.join(args.bitmap, f"{img['n']:03d}-r{role}.png"), w, h, img["buf"])
                elif kind == HELLO_ACK:
                    print(f"{t:6.2f} HELLO_ACK", flush=True)

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    silence = b"\0" * FRAME
    plan = [silence] * 50  # 1 秒静音
    plan += [speech[i:i + FRAME].ljust(FRAME, b"\0") for i in range(0, len(speech), FRAME)]
    start = time.time()
    i = 0
    while time.time() - t0 < args.seconds:
        chunk = plan[i] if i < len(plan) else silence
        send(PCM, chunk)
        i += 1
        delay = start + i * 0.02 - time.time()
        if delay > 0:
            time.sleep(delay)
    stop[0] = True
    s.close()

    ts = stats["pcm_t"]
    if ts:
        span = ts[-1] - ts[0]
        print(f"收到 PCM {len(ts)} 帧（{len(ts) * 0.02:.1f} 秒声音），跨度 {span:.1f} 秒，"
              f"平均 {len(ts) / max(span, 0.001):.0f} 帧/秒", flush=True)
        bursts = sum(1 for a, b in zip(ts, ts[1:]) if b - a < 0.002)
        print(f"间隔小于 2 ms 的帧对：{bursts}", flush=True)
    if img["n"]:
        print(f"字幕位图提交 {img['n']} 次，共 {img['bytes']} 字节（{img['bytes'] * 8 / max(1, time.time() - t0) / 1000:.1f} kbps）", flush=True)


if __name__ == "__main__":
    sys.exit(main())
