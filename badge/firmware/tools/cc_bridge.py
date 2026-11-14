#!/usr/bin/env python3
"""cc_bridge.py — AI Passport 的 USB 调试与联网桥（配套固件 main/cc_bridge.c）。

作用：
  1. 把设备的 "@CC POST ..." 请求转发到中转服务（流式），响应分块送回设备；
  2. 同步时间、发心跳；
  3. 从命令文件读取调试命令：注入按键、截屏、查内存、重启；
  4. 把设备串口日志带时间戳写进日志文件。

环境变量：
  PASSPORT_API_BASE   中转服务地址，例如 https://relay.example.com
  PASSPORT_API_KEY    中转服务 key（只在本进程里，作为 x-api-key 请求头；不发给设备，不写日志）
  PASSPORT_MODEL      可选，覆盖设备请求里的 model

用法：
  cc_bridge.py --port /dev/cu.usbmodemXXXX --log bridge.log --cmd bridge.cmd
命令文件里每追加一行执行一次：
  key <up|down|ok> <press|click|double|long|release>
  shot <out.png>
  mem
  reboot
  quit
"""
import argparse
import base64
import http.client
import json
import os
import struct
import sys
import threading
import time
import urllib.parse
import zlib

import serial  # pyserial（ESP-IDF 的 Python 环境里自带）


class Bridge:
    def __init__(self, args):
        self.args = args
        self.ser = serial.Serial()
        self.ser.port = args.port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.1
        self.ser.dtr = False  # 不要因为打开串口把芯片复位
        self.ser.rts = False
        self.ser.open()
        self.wlock = threading.Lock()
        self.log = open(args.log, "a", buffering=1, encoding="utf-8")
        self.reqs = {}      # id -> {"path", "headers", "body"}
        self.cancelled = set()
        self.shot_rows = None
        self.shot_path = None
        self.stop = False
        self.t0 = time.time()

    # ---- 基础 ----
    def note(self, msg):
        self.log.write(f"{time.time() - self.t0:9.3f} [bridge] {msg}\n")

    def send(self, line):
        with self.wlock:
            self.ser.write(line.encode("ascii") + b"\n")
            self.ser.flush()

    def sync_time(self):
        offset = -time.timezone if not time.localtime().tm_isdst else -time.altzone
        hours = offset // 3600
        tz = f"UTC{-hours:+d}" if hours else "UTC0"  # POSIX TZ 的符号与常识相反
        self.send(f"@T {int(time.time())} {tz}")

    # ---- HTTP 转发 ----
    def relay(self, rid, req):
        base = os.environ.get("PASSPORT_API_BASE", "").rstrip("/")
        key = os.environ.get("PASSPORT_API_KEY", "")
        status, err = 0, 0
        t0 = time.time()
        nbytes = 0
        try:
            if not base or not key:
                raise RuntimeError("PASSPORT_API_BASE / PASSPORT_API_KEY 未设置")
            body = req["body"]
            model = os.environ.get("PASSPORT_MODEL")
            if model:
                obj = json.loads(body)
                obj["model"] = model
                body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            u = urllib.parse.urlparse(base + req["path"])
            cls = http.client.HTTPSConnection if u.scheme == "https" else http.client.HTTPConnection
            conn = cls(u.netloc, timeout=90)
            headers = {}
            for h in req["headers"]:
                name, _, value = h.partition(":")
                if name.strip().lower() in ("x-api-key", "authorization"):
                    continue  # 鉴权只由主机注入
                headers[name.strip()] = value.strip()
            headers["x-api-key"] = key
            conn.request("POST", u.path, body=body, headers=headers)
            resp = conn.getresponse()
            status = resp.status
            while True:
                chunk = resp.read1(2048)
                if not chunk or rid in self.cancelled:
                    break
                nbytes += len(chunk)
                for i in range(0, len(chunk), 96):
                    self.send(f"@D {rid} {base64.b64encode(chunk[i:i + 96]).decode()}")
                    time.sleep(0.004)  # 设备串口接收缓冲 4 KB，别冲太猛
            conn.close()
        except Exception as e:  # noqa: BLE001
            err = -1
            self.note(f"relay {rid} 失败: {type(e).__name__}: {e}")
        self.send(f"@E {rid} {status} {err}")
        self.note(f"relay {rid} {req['path']} status={status} err={err} bytes={nbytes} {time.time() - t0:.2f}s")

    # ---- 设备行 ----
    def handle_cc(self, parts):
        cmd = parts[1] if len(parts) > 1 else ""
        if cmd == "HELLO":
            self.sync_time()
            self.send("@P")
        elif cmd == "POST" and len(parts) >= 4:
            self.reqs[parts[2]] = {"path": parts[3], "headers": [], "body": b""}
        elif cmd == "H" and len(parts) >= 4 and parts[2] in self.reqs:
            r = self.reqs[parts[2]]
            r.setdefault("hbuf", b"")
            r["hbuf"] += base64.b64decode(parts[3])
            # 一个头可能被拆成多行；头里不含换行，短于一个分块时即结束
            if len(base64.b64decode(parts[3])) < 144:
                r["headers"].append(r.pop("hbuf").decode("utf-8"))
        elif cmd == "B" and len(parts) >= 4 and parts[2] in self.reqs:
            self.reqs[parts[2]]["body"] += base64.b64decode(parts[3])
        elif cmd == "GO" and len(parts) >= 4 and parts[2] in self.reqs:
            r = self.reqs.pop(parts[2])
            if "hbuf" in r:
                r["headers"].append(r.pop("hbuf").decode("utf-8"))
            if len(r["body"]) != int(parts[3]):
                self.note(f"请求 {parts[2]} 长度不符: {len(r['body'])} != {parts[3]}")
                self.send(f"@E {parts[2]} 0 -2")
                return
            threading.Thread(target=self.relay, args=(parts[2], r), daemon=True).start()
        elif cmd == "CANCEL" and len(parts) >= 3:
            self.cancelled.add(parts[2])
        elif cmd == "SHOT" and len(parts) >= 3:
            if parts[2] == "BEGIN":
                self.shot_rows = {}
            elif parts[2] == "END" and self.shot_rows is not None:
                self.save_shot()
        elif cmd == "PX" and len(parts) >= 4 and self.shot_rows is not None:
            y = int(parts[2])
            self.shot_rows[y] = self.shot_rows.get(y, b"") + base64.b64decode(parts[3])

    def save_shot(self):
        rows, self.shot_rows = self.shot_rows, None
        w, h = 240, 320
        raw = bytearray()
        missing = 0
        for y in range(h):
            row = rows.get(y, b"")
            if len(row) != w * 2:
                missing += 1
                row = b"\x00" * (w * 2)
            raw.append(0)
            for x in range(w):
                v = row[2 * x] | (row[2 * x + 1] << 8)  # RGB565 小端
                r5, g6, b5 = v >> 11, (v >> 5) & 0x3F, v & 0x1F
                raw += bytes(((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
        path = self.shot_path or "shot.png"
        with open(path, "wb") as f:
            f.write(png)
        self.note(f"截屏已保存 {path} 缺行={missing}")

    # ---- 命令文件 ----
    def handle_cmd(self, line):
        p = line.split()
        if not p:
            return
        self.note(f"cmd: {line}")
        if p[0] == "key" and len(p) == 3:
            self.send(f"@K {p[1]} {p[2]}")
        elif p[0] == "shot" and len(p) == 2:
            self.shot_path = p[1]
            self.send("@S")
        elif p[0] == "mem":
            self.send("@M")
        elif p[0] == "reboot":
            self.send("@R")
        elif p[0] == "quit":
            self.stop = True

    def run(self):
        open(self.args.cmd, "a").close()
        cmd_f = open(self.args.cmd, "r", encoding="utf-8")
        cmd_f.seek(0, os.SEEK_END)
        buf = b""
        last_ping = 0.0
        self.note(f"bridge 启动 port={self.args.port}")
        self.sync_time()
        while not self.stop:
            now = time.time()
            if now - last_ping >= 2.0:
                last_ping = now
                self.send("@P")
            try:
                data = self.ser.read(4096)
            except serial.SerialException as e:
                self.note(f"串口断开: {e}")
                break
            if data:
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    text = raw.decode("utf-8", "replace").rstrip("\r")
                    if text.startswith("@CC "):
                        parts = text.split(" ")
                        if parts[1] not in ("PX", "B"):  # 像素和请求体不进日志
                            self.log.write(f"{time.time() - self.t0:9.3f} {text[:160]}\n")
                        try:
                            self.handle_cc(parts)
                        except Exception as e:  # noqa: BLE001
                            self.note(f"协议行处理失败: {e}")
                    elif text:
                        self.log.write(f"{time.time() - self.t0:9.3f} {text}\n")
            for line in cmd_f.readlines():
                self.handle_cmd(line.strip())
        self.note("bridge 退出")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--log", default="bridge.log")
    ap.add_argument("--cmd", default="bridge.cmd")
    Bridge(ap.parse_args()).run()


if __name__ == "__main__":
    sys.exit(main())
