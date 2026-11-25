#!/usr/bin/env python3
"""把 dsh-pet 的 webm 动画转成设备可播的 NVA2 高清帧序列包。

改进要点：
  1. 去除旧版对调色板索引执行 MedianFilter(3) 导致的线条/五官糊化问题；
  2. 基于 640x360 统一世界坐标按目标站立身高（默认 162px）等比缩放，消除运行时最近邻缩放锯齿；
  3. 预乘叠加纯黑 OLED 背景并保留柔和抗锯齿边缘（alpha > 26）；
  4. 96 色调色板 + 腮红/金边/高光加权采样，恢复少女腮红与发丝细节；
  5. NVA2 支持逐帧时空稳定与 XOR 帧间差分 + zlib-9，体积比 NVA1 更小且无闪烁噪点。

用法：
    python3 convert_pet.py --src ../dsh-pet/dsh-pet/assets/webm --out assets/pets/nova --preview
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
from PIL import Image

DEFAULT_IDLE_H = 162
DEFAULT_COLORS = 96
DEFAULT_FPS = 10
ALPHA_CUTOFF = 26
KEYFRAME_INTERVAL = 25
# 时空稳帧阈值：相邻帧同一像素的 max(RGB) 差 ≤ 阈值就沿用上一帧的调色板索引。
# 24 个高清包在 7 的时候是 12.4MB，塞不进 12MB 的 spiffs 分区；抬到 12 后
# 10.65MB（省 16%），肉眼看不出差别 —— 它只压掉抗锯齿边缘与渐变的逐帧抖动，
# 不改变轮廓与动作。关键帧（第 0 帧、每 25 帧）不受影响；但非关键帧会被
# AOD 定格取用，所以 3 张 page_aod* 像素基线随本次重转更新过。
STAB_DIFF_MAX = 12

# 核心动画：设备侧状态机用得到的那些，按 dsh-pet config.jsonc 的 workStatus 语义对齐。
# 顺序 = src/anim/nva.h 的 NvaAnimKey 0..23（前 9 项与旧版 100% 兼容），
# 改这里必须同步 src/anim/nva.h、src/net/proto.cpp、host-plugin/lib/ai.js。
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
    "click_happy": "点击回应-开心跃动",
    "click_shy": "点击回应-害羞惊讶",
    "click_angry": "点击回应-傲娇生气",
    "click_giggle": "点击回应-挠痒咯咯笑",
    "crab_walk": "螃蟹走路",
    "happy_run": "开心小跑",
    "coding": "写代码",
    "archive": "工作状态-清点归档",
    "stretch": "超大伸懒腰",
    "yawn": "哈欠连天",
    "wake_up": "打瞌睡被惊醒",
    "hum": "悠闲哼歌",
    "eat_token": "吃Token",
    "bubble": "鲸鱼吐泡泡特效",
    "quota_worry": "余额-数金皱眉",
}


def decode(webm: str, fps: int, tmpdir: str) -> list[np.ndarray]:
    """ffmpeg 抽帧。必须显式指定 libvpx-vp9 解码器，否则 alpha 全是 255。"""
    pat = os.path.join(tmpdir, "f_%05d.png")
    subprocess.run(
        [
            "ffmpeg", "-v", "error",
            "-c:v", "libvpx-vp9",
            "-i", webm,
            "-vf", f"fps={fps}",
            "-pix_fmt", "rgba", pat, "-y",
        ],
        check=True,
    )
    files = sorted(glob.glob(os.path.join(tmpdir, "f_*.png")))
    return [np.array(Image.open(f).convert("RGBA")).astype(np.float32) for f in files]


def union_bbox(frames: list[np.ndarray], thresh: int = 16) -> tuple[int, int, int, int]:
    """全动画的联合包围盒，保证角色位置不跳。"""
    box = None
    for a in frames:
        ys, xs = np.where(a[:, :, 3] > thresh)
        if len(xs) == 0:
            continue
        b = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
        box = b if box is None else (
            min(box[0], b[0]), min(box[1], b[1]),
            max(box[2], b[2]), max(box[3], b[3]),
        )
    if box is None:
        raise RuntimeError("empty alpha bounding box")
    return box


def resize_frames_black_matte(
    frames: list[np.ndarray],
    box: tuple[int, int, int, int],
    scale: float,
) -> tuple[list[np.ndarray], int, int]:
    """预乘叠加 OLED 纯黑底色后再缩放，边缘抗锯齿自然融入黑底且无灰白毛边。"""
    bw, bh = box[2] - box[0], box[3] - box[1]
    dw = max(2, round(bw * scale))
    dh = max(2, round(bh * scale))
    out = []
    for a in frames:
        c = a[box[1]:box[3], box[0]:box[2]]
        alpha = c[:, :, 3:4] / 255.0
        # On pure black COL_BG (0,0,0), premultiplied RGB IS the exact blended pixel!
        premul = np.concatenate([c[:, :, :3] * alpha, c[:, :, 3:4]], axis=-1)
        small = Image.fromarray(np.clip(premul, 0, 255).astype(np.uint8)).resize(
            (dw, dh), Image.LANCZOS
        )
        out.append(np.array(small).astype(np.float32))
    return out, dw, dh


def build_palette(crops: list[np.ndarray], n_colors: int) -> np.ndarray:
    """对高饱和/暖色像素（腮红、金饰、青色高光）加权采样，防止小面积点缀被主色调吞掉。"""
    opaque = np.concatenate([c[:, :, :3][c[:, :, 3] > ALPHA_CUTOFF] for c in crops])
    mx = opaque.max(axis=-1)
    mn = opaque.min(axis=-1)
    sat = mx - mn
    warm_or_vivid = opaque[(opaque[:, 0] > opaque[:, 2] + 10) | (sat > 55)]
    if len(warm_or_vivid) > 0:
        sample = np.concatenate([opaque, warm_or_vivid, warm_or_vivid, warm_or_vivid], axis=0)
    else:
        sample = opaque
    q = Image.fromarray(sample.reshape(-1, 1, 3).astype(np.uint8)).quantize(
        colors=n_colors, method=Image.Quantize.MEDIANCUT
    )
    return np.array(q.getpalette()[: n_colors * 3]).reshape(-1, 3).astype(np.int32)


def quantize_stabilized(
    crops: list[np.ndarray],
    palette: np.ndarray,
    stab_diff: int = STAB_DIFF_MAX,
) -> list[np.ndarray]:
    """用 int32 精确最近邻量化，并结合时空稳帧阈值消除相邻帧微弱噪点闪烁。"""
    frames = []
    prev_rgb = None
    prev_idx = None
    for c in crops:
        rgb = c[:, :, :3].astype(np.int32)
        d = ((rgb[:, :, None, :] - palette[None, None, :, :]) ** 2).sum(-1)
        idx = np.where(
            c[:, :, 3] > ALPHA_CUTOFF, d.argmin(-1).astype(np.uint8) + 1, 0
        ).astype(np.uint8)
        if prev_idx is not None and stab_diff > 0:
            same_vis = (idx > 0) == (prev_idx > 0)
            diff = np.abs(rgb - prev_rgb).max(axis=-1)
            idx = np.where(same_vis & (diff <= stab_diff), prev_idx, idx)
        frames.append(idx)
        prev_rgb = rgb
        prev_idx = idx
    return frames


def encode_pack_nva2(
    name: str,
    frames: list[np.ndarray],
    palette: np.ndarray,
    fps: int,
    out_dir: str,
) -> tuple[str, int, int, int, int]:
    """NVA2 格式：
       magic 'NVA2' | u16 w | u16 h | u16 nframes | u8 ncolors | u8 fps
       palette RGB888[ncolors]
       offsets u32[nframes+1]
       blobs: 每帧 1 字节模式标志 (0x00=关键帧zlib, 0x01=与上一帧XOR差分zlib) + 压缩数据
    """
    h, w = frames[0].shape[0], frames[0].shape[1]
    blobs = []
    for i, f in enumerate(frames):
        if i % KEYFRAME_INTERVAL == 0:
            blobs.append(b"\x00" + zlib.compress(f.tobytes(), 9))
        else:
            delta = np.bitwise_xor(f, frames[i - 1])
            blobs.append(b"\x01" + zlib.compress(delta.tobytes(), 9))

    offs = [0]
    for b in blobs:
        offs.append(offs[-1] + len(b))
    header = b"NVA2" + np.array([w, h, len(frames)], dtype="<u2").tobytes()
    header += bytes([len(palette), fps])
    header += palette.astype(np.uint8).tobytes()
    header += np.array(offs, dtype="<u4").tobytes()
    path = os.path.join(out_dir, name + ".nva")
    with open(path, "wb") as f:
        f.write(header)
        for b in blobs:
            f.write(b)
    return path, w, h, len(frames), os.path.getsize(path)


def convert_one(
    webm: str,
    name: str,
    out_dir: str,
    scale: float,
    colors: int,
    fps: int,
    keep_png: bool,
) -> dict:
    tmp = tempfile.mkdtemp(prefix="nva2_")
    try:
        raw = decode(webm, fps, tmp)
        if not raw:
            raise RuntimeError("no frames decoded")
        b0 = union_bbox(raw[:1])
        box = union_bbox(raw)
        if name == "quota":
            cx0 = (b0[0] + b0[2]) // 2
            box = (max(box[0], cx0 - 170), box[1], min(box[2], cx0 + 170), box[3])
        crops, w, h = resize_frames_black_matte(raw, box, scale)
        palette = build_palette(crops, colors)
        frames = quantize_stabilized(crops, palette)

        _, w, h, n, size = encode_pack_nva2(name, frames, palette, fps, out_dir)

        if keep_png:
            step = max(1, n // 6)
            picks = [i for i in range(0, n, step)][:6]
            sheet = Image.new("RGB", (w * len(picks), h), (0, 0, 0))
            palette8 = palette.astype(np.uint8)
            for slot, fi in enumerate(picks):
                comp = np.zeros((h, w, 3), np.uint8)
                m = frames[fi] > 0
                comp[m] = palette8[frames[fi][m] - 1]
                sheet.paste(Image.fromarray(comp), (slot * w, 0))
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


def measure_idle_native_height(src_dir: str) -> int:
    webm = os.path.join(src_dir, CORE["idle"] + ".webm")
    tmp = tempfile.mkdtemp(prefix="nva_idle_")
    try:
        raw = decode(webm, 4, tmp)
        box = union_bbox(raw)
        return box[3] - box[1]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="dsh-pet/assets/webm 目录")
    ap.add_argument("--out", required=True)
    ap.add_argument("--idle-h", type=int, default=DEFAULT_IDLE_H)
    ap.add_argument("--colors", type=int, default=DEFAULT_COLORS)
    ap.add_argument("--fps", type=int, default=DEFAULT_FPS)
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--only", default="", help="逗号分隔，只转这些 key")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    idle_native_h = measure_idle_native_height(args.src)
    scale = float(args.idle_h) / float(idle_native_h)
    print(f"World scale: {scale:.4f} (idle native h={idle_native_h} -> {args.idle_h}px)")

    keys = [k.strip() for k in args.only.split(",") if k.strip()] or list(CORE)
    results = []
    for key in keys:
        src_name = CORE.get(key, key)
        webm = os.path.join(args.src, src_name + ".webm")
        if not os.path.exists(webm):
            print(f"SKIP {key}: {webm} 不存在", file=sys.stderr)
            continue
        r = convert_one(webm, key, args.out, scale, args.colors, args.fps, args.preview)
        print(
            f"{r['name']:10s} {r['w']}x{r['h']} {r['frames']:3d}帧 "
            f"{r['kb']:7.1f}KB  ({r['kb_per_frame']:.2f}KB/帧)"
        )
        results.append(r)

    total = sum(r["bytes"] for r in results)
    manifest = {
        "generated_at": "2026-11-21",
        "format": "NVA2",
        "source": "PC2005-cloud/dsh-pet",
        "source_license": "素材 CC BY-NC-SA 4.0（非商业）",
        "params": {
            "idle_h": args.idle_h,
            "world_scale": round(scale, 4),
            "colors": args.colors,
            "fps": args.fps,
            "median": False,
            "xor_delta": True,
            "stab_diff": STAB_DIFF_MAX,
            "keyframe_interval": KEYFRAME_INTERVAL,
        },
        "anims": results,
        "total_bytes": total,
        "total_kb": round(total / 1024, 1),
    }
    with open(os.path.join(args.out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
    print(f"\n合计 {len(results)} 个动画 / {total/1024:.1f} KB")


if __name__ == "__main__":
    main()
