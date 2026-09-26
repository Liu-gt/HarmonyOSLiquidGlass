#!/usr/bin/env python3
"""将输入图片转换为原生层直接采样的 RGBA 数据（前缀 8 字节为 u32 LE 宽、高）。

预处理优势：
- 避免原生渲染层依赖图片解码 NDK，简化依赖与降低失败面
- 运行时直接进行内存映射与纹理上传（glTexImage2D），性能与确定性更高

用法:
    python3 tools/make-photo-raw.py <input-image> [target-width]

输出:
    entry/src/main/resources/rawfile/photo.rgba
"""
import struct
import sys
from pathlib import Path

from PIL import Image

if len(sys.argv) < 2:
    sys.exit("usage: python3 tools/make-photo-raw.py <input-image> [target-width]")

root = Path(__file__).resolve().parent.parent
src = Path(sys.argv[1])
dst = root / "entry/src/main/resources/rawfile/photo.rgba"
target_w = int(sys.argv[2]) if len(sys.argv) > 2 else 1024

img = Image.open(src).convert("RGBA")
if img.width != target_w:
    h = round(img.height * target_w / img.width)
    img = img.resize((target_w, h), Image.LANCZOS)

dst.parent.mkdir(parents=True, exist_ok=True)
with dst.open("wb") as f:
    f.write(struct.pack("<II", img.width, img.height))
    f.write(img.tobytes())

print(f"{src.name} -> {dst.relative_to(root)} {img.width}x{img.height} "
      f"({dst.stat().st_size / 1024 / 1024:.1f} MB)")
