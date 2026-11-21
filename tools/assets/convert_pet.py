#!/usr/bin/env python3
"""把 dsh-pet 的 webm 动画转成设备可播的帧序列包。

思路（参数扫描结论见 docs/NOVA-BUDDY-PLAN.md §9.2）：
  量化到 16 色调色板 → 索引图（0 = 透明）→ 逐帧 zlib(deflate) → 打成一个 .nva 包

不用 RLE：素材有大量渐变与高光，一帧 19,581 种颜色，RLE 只压 1.7x。
不用帧间差分：相邻帧差异 2637 像素、压缩后 6.6 KB/帧，比整帧 4.1 KB 还大。

用法：
    python3 convert_pet.py --src ../dsh-pet/dsh-pet/assets/webm --out ../assets/pets/nova
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zlib

import numpy as np
from PIL import Image, ImageFilter

# 设备画布 184×224，角色占 128×156，上下留白给状态条与粒子
DEFAULT_WIDTH = 128
DEFAULT_MAX_H = 208
DEFAULT_COLORS = 16
DEFAULT_FPS = 12
ALPHA_CUTOFF = 128  # 低于这个 alpha 视为透明（调色板索引 0）

# 核心动画：设备侧状态机用得到的那些，按 dsh-pet config.jsonc 的 workStatus 语义对齐
CORE = {
    "idle": "待机呼吸休闲",
    "thinking": "工作状态-思考冒泡",
    "working": "工作状态-忙碌点按",
    "waiting": "工作状态-原地踱步张望",
    "success": "工作状态-雀跃庆祝",
    "error": "工作状态-垂头叹气冒汗",
    "click": "点击回应-元气挥手",
    "sleep": "睡觉-循环",
    "quota": "余额-钱袋满溢",
}


def decode(webm, fps, tmpdir):
    """ffmpeg 抽帧。必须显式指定 libvpx-vp9 解码器，否则 alpha 全是 255。"""
    pat = os.path.join(tmpdir, "f_%05d.png")
    # 源 24fps → 目标 12fps
    step = max(1, round(24 / fps))
    subprocess.run(
        [
            "ffmpeg", "-v", "error",
            "-c:v", "libvpx-vp9",          # ← 关键：默认解码器丢 alpha
            "-i", webm,
            "-vf", f"select='not(mod(n\\,{step}))'",
            "-vsync", "0", "-pix_fmt", "rgba", pat, "-y",
        ],
        check=True,
    )
    files = sorted(glob.glob(os.path.join(tmpdir, "f_*.png")))
    return [np.array(Image.open(f).convert("RGBA")).astype(np.float32) for f in files]


def union_bbox(frames, thresh=8):
    """全动画的联合包围盒，保证角色位置不跳。"""
    box = None
    for a in frames:
        ys, xs = np.where(a[:, :, 3] > thresh)
        b = (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1)
        box = b if box is None else (
            min(box[0], b[0]), min(box[1], b[1]),
            max(box[2], b[2]), max(box[3], b[3]),
        )
    return box


def resize_frames(frames, box, tw, max_h):
    """alpha 预乘 → 缩放 → 反预乘。不预乘会让透明区渗黑边。"""
    bw, bh = box[2] - box[0], box[3] - box[1]
    scale = min(tw / bw, max_h / bh)
    dw, dh = round(bw * scale), round(bh * scale)
    out = []
    for a in frames:
        c = a[box[1]:box[3], box[0]:box[2]]
        alpha = c[:, :, 3:4] / 255.0
        premul = np.concatenate([c[:, :, :3] * alpha, c[:, :, 3:4]], -1)
        small = Image.fromarray(premul.astype(np.uint8)).resize((dw, dh), Image.LANCZOS)
        r = np.array(small).astype(np.float32)
        a2 = r[:, :, 3:4] / 255.0
        rgb = np.where(a2 > 1e-3, r[:, :, :3] / np.maximum(a2, 1e-3), 0)
        out.append(np.concatenate([np.clip(rgb, 0, 255), r[:, :, 3:4]], -1))
    return out, dw, dh


def build_palette(crops, n):
    opaque = np.concatenate([c[:, :, :3][c[:, :, 3] > ALPHA_CUTOFF] for c in crops])
    q = Image.fromarray(opaque.reshape(-1, 1, 3).astype(np.uint8)).quantize(
        colors=n, method=Image.MEDIANCUT
    )
    return np.array(q.getpalette()[: n * 3]).reshape(-1, 3).astype(np.int32)


def quantize(crops, palette, median=True):
    """距离计算必须用 int32：int16 的平方会溢出，画面变噪点。"""
    frames = []
    for c in crops:
        rgb = c[:, :, :3].astype(np.int32)
        d = ((rgb[:, :, None, :] - palette[None, None, :, :]) ** 2).sum(-1)
        idx = np.where(
            c[:, :, 3] > ALPHA_CUTOFF, d.argmin(-1).astype(np.uint8) + 1, 0
        ).astype(np.uint8)
        im = Image.fromarray(idx)
        if median:
            im = im.filter(ImageFilter.MedianFilter(3))
        frames.append(np.array(im))
    return frames


def encode_pack(name, frames, palette, fps, out_dir):
    """自定格式 .nva：
       magic 'NVA1' | u16 w | u16 h | u16 nframes | u8 ncolors | fps u8
       palette RGB888[ncolors]
       offsets u32[nframes+1] | zlib blobs
    """
    w, h = frames[0].shape[1], frames[0].shape[0]
    blobs = [zlib.compress(f.tobytes(), 9) for f in frames]
    offs = [0]
    for b in blobs:
        offs.append(offs[-1] + len(b))
    header = b"NVA1" + np.array([w, h, len(frames)], dtype="<u2").tobytes()
    header += bytes([len(palette), fps])
    header += palette.astype(np.uint8).tobytes()
    header += np.array(offs, dtype="<u4").tobytes()
    path = os.path.join(out_dir, name + ".nva")
    with open(path, "wb") as f:
        f.write(header)
        for b in blobs:
            f.write(b)
    return path, w, h, len(frames), os.path.getsize(path)


def convert_one(webm, name, out_dir, width, max_h, colors, fps, median, keep_png):
    tmp = tempfile.mkdtemp(prefix="nva_")
    try:
        raw = decode(webm, fps, tmp)
        if not raw:
            raise RuntimeError("no frames decoded")
        box = union_bbox(raw)
        crops, w, h = resize_frames(raw, box, width, max_h)
        palette = build_palette(crops, colors)
        frames = quantize(crops, palette, median)

        path, w, h, n, size = encode_pack(name, frames, palette, fps, out_dir)

        if keep_png:
            sheet = Image.new("RGB", (w * min(n, 6), h), (18, 18, 18))
            palette8 = palette.astype(np.uint8)
            for i in range(min(n, 6)):
                comp = np.zeros((h, w, 3), np.uint8)
                m = frames[i] > 0
                comp[m] = palette8[frames[i][m] - 1]
                sheet.paste(Image.fromarray(comp), (i * w, 0))
            sheet.save(os.path.join(out_dir, name + "_preview.png"))

        return {
            "name": name,
            "w": w, "h": h, "frames": n,
            "fps": fps, "colors": colors,
            "bytes": size,
            "kb": round(size / 1024, 1),
            "kb_per_frame": round(size / n / 1024, 2),
        }
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="dsh-pet/assets/webm 目录")
    ap.add_argument("--out", required=True)
    ap.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    ap.add_argument("--max-h", type=int, default=DEFAULT_MAX_H)
    ap.add_argument("--colors", type=int, default=DEFAULT_COLORS)
    ap.add_argument("--fps", type=int, default=DEFAULT_FPS)
    ap.add_argument("--no-median", action="store_true")
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--only", default="", help="逗号分隔，只转这些 key")
    ap.add_argument("--all", action="store_true", help="转 CORE 全集（默认就是）")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    keys = [k.strip() for k in args.only.split(",") if k.strip()] or list(CORE)
    results = []
    for key in keys:
        src_name = CORE.get(key, key)
        webm = os.path.join(args.src, src_name + ".webm")
        if not os.path.exists(webm):
            print(f"SKIP {key}: {webm} 不存在", file=sys.stderr)
            continue
        r = convert_one(
            webm, key, args.out, args.width, args.max_h,
            args.colors, args.fps, not args.no_median, args.preview,
        )
        print(
            f"{r['name']:10s} {r['w']}x{r['h']} {r['frames']:3d}帧 "
            f"{r['kb']:7.1f}KB  ({r['kb_per_frame']:.2f}KB/帧)"
        )
        results.append(r)

    total = sum(r["bytes"] for r in results)
    manifest = {
        "generated_at": "2026-11-21",
        "source": "PC2005-cloud/dsh-pet",
        "source_license": "素材 CC BY-NC-SA 4.0（非商业）",
        "params": {
            "width": args.width, "max_h": args.max_h,
            "colors": args.colors, "fps": args.fps,
            "median": not args.no_median,
        },
        "anims": results,
        "total_bytes": total,
        "total_kb": round(total / 1024, 1),
    }
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
    print(f"\n合计 {len(results)} 个动画 / {total/1024:.1f} KB")


if __name__ == "__main__":
    main()
