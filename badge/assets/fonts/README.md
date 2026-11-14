# Noto Sans SC GB2312 子集

供模拟器和以后的真机 TinyTTF 使用。SIL Open Font License 1.1，全文见同目录 `OFL.txt`。

- 来源：<https://github.com/google/fonts/tree/main/ofl/notosanssc> 里的可变字体 `NotoSansSC[wght].ttf`
- 上游版本：name 表 `Version 2.004-H2`（Google Fonts `METADATA.pb` 记录的 noto-cjk 提交 `523d033d6cb47f4a80c58a35753646f5c3608a78`）
- 可变字体默认实例是 Thin（wght=100）。生成时用 `fontTools.varLib.instancer` 固定成 Regular（wght=400），再子集
- 字符：Python `gb2312` 编解码得到的全部 6763 个汉字，加上 GB2312 里其余符号、ASCII 可打印字符，以及 … — ～ · 「」『』【】。合计 7542 个字符，8202 个字形
- 轮廓：TrueType glyf。已去掉 hinting（不含 fpgm、prep、cvt、gasp）
- 文件：`NotoSansSC-GB2312.ttf`，2432168 字节

生成（在仓库根目录，先 `python3 -m venv tools/fonts/.venv && tools/fonts/.venv/bin/pip install fonttools`，并把可变字体放到 `tools/fonts/upstream/`）：

```bash
tools/fonts/.venv/bin/python -I tools/fonts/build_subset.py \
  "tools/fonts/upstream/NotoSansSC[wght].ttf" \
  assets/fonts/NotoSansSC-GB2312.ttf \
  --pyftsubset tools/fonts/.venv/bin/pyftsubset
```

`build_subset.py` 会先实例化 Regular，再调用 `pyftsubset`，参数是 `--no-hinting --glyph-names --legacy-cmap --symbol-cmap --notdef-glyph --notdef-outline --recommended-glyphs --name-IDs=* --drop-tables+=DSIG,gasp --layout-features=*`。
