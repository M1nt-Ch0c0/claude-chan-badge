#!/usr/bin/env python3
"""make_sprites.py — 把 Claude娘 的 Q 版参考图转成 LVGL 9 的 RGB565A8 图像数组。

输入：mac/VoiceDesk/Sources/Resources/ui/img/*.png（Mac 端也用这几张）
输出：badge/app/app/cc_sprites.c / cc_sprites.h，以及一张预览图。
用法：python3 badge/app/tools/make_sprites.py（需要 Pillow）
"""
import os
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
GEN = os.path.join(ROOT, "mac", "VoiceDesk", "Sources", "Resources", "ui", "img")
OUT = os.path.join(ROOT, "badge", "app", "app")
FULL_H = 236   # 整张图缩放到这个高度
CROP_H = 164   # 只保留上半身（下面被对话框挡住）
SPRITES = [
    ("listen", "listen.png"),
    ("speak", "speak.png"),
    ("think", "think.png"),
    ("sleep", "sleep.png"),
    ("worry", "worry.png"),
    ("cheer", "cheer.png"),
]


def convert(name, fn):
    im = Image.open(os.path.join(GEN, fn)).convert("RGBA")
    w = round(im.width * FULL_H / im.height)
    # 预乘后缩放，避免透明边缘发黑
    im = im.convert("RGBa").resize((w, FULL_H), Image.LANCZOS).convert("RGBA")
    if im.width > 220:
        h = round(im.height * 220 / im.width)
        im = im.resize((220, h), Image.LANCZOS)
    if im.height > CROP_H:
        im = im.crop((0, 0, im.width, CROP_H))
    px = im.load()
    rgb, alpha = bytearray(), bytearray()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            rgb += bytes((v & 0xFF, v >> 8))
            alpha.append(a)
    return name, im.width, im.height, bytes(rgb + alpha), im


def main():
    items = [convert(n, f) for n, f in SPRITES]
    with open(os.path.join(OUT, "cc_sprites.h"), "w") as h:
        h.write("// 由 tools/make_sprites.py 生成，请勿手改。\n#pragma once\n#include \"lvgl.h\"\n\n")
        for n, w, hh, _, _ in items:
            h.write(f"extern const lv_image_dsc_t cc_sprite_{n};  // {w}x{hh}\n")
    with open(os.path.join(OUT, "cc_sprites.c"), "w") as c:
        c.write("// 由 tools/make_sprites.py 生成，请勿手改。角色设定图由用户提供。\n#include \"cc_sprites.h\"\n\n")
        for n, w, hh, data, _ in items:
            c.write(f"static const uint8_t {n}_map[] = {{\n")
            for i in range(0, len(data), 24):
                c.write("    " + ",".join(str(b) for b in data[i:i + 24]) + ",\n")
            c.write("};\n\n")
            c.write(f"const lv_image_dsc_t cc_sprite_{n} = {{\n"
                    f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, .w = {w}, .h = {hh}, .stride = {w * 2}}},\n"
                    f"    .data_size = sizeof({n}_map),\n    .data = {n}_map,\n}};\n\n")
    # 预览：三个姿势贴在应用背景色上
    prev = Image.new("RGBA", (240 * len(items), 320), (255, 236, 220, 255))
    for i, (_, w, _, _, im) in enumerate(items):
        prev.alpha_composite(im, (240 * i + (240 - w) // 2, 24))
    prev.convert("RGB").save(os.path.join(ROOT, "badge", "app", "sprites-preview.png"))
    for n, w, hh, data, _ in items:
        print(n, w, hh, len(data), "bytes")


if __name__ == "__main__":
    main()
