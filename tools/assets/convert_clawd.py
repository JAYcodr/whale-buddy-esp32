#!/usr/bin/env python3
"""把 克码人生 · Clawd (cc-clawd) 精灵表转成设备可播的 NVA2 帧序列包。

特性（按 docs/CLAWD-HANDOFF.md 约束实现）：
  1. 解析 pack.json，在 144x144 标准画布按 box.x/box.y 复原每一帧；
  2. 以 anchor.x (72) 为中心对称裁边，底部统一对齐画布底边 (144) 以确保切动作不横跳/穿地；
  3. 使用 NEAREST 最近邻缩放保留硬朗像素边（默认 1.5x -> 最大 207x216，满足 NVA2 <= 49,280 像素及 <240px DMA 快传路径）；
  4. 按固定整型 fps（默认 10 fps = 100ms）重采样可变帧时长；
  5. 直接复用源调色板（<= 255 色免重新量化失真）并写 NVA2 XOR 差分 + zlib-9。

用法：
  python3 tools/assets/convert_clawd.py --src /path/to/clawd_raw --out assets/pets/clawd --preview
"""

import argparse
import json
import os
import sys
import zlib

import numpy as np
from PIL import Image, ImageDraw

DEFAULT_FPS = 10
DEFAULT_SCALE = 1.5
KEYFRAME_INTERVAL = 25
MAX_FRAME_PIXELS = 220 * 224
ALPHA_CUTOFF = 128  # Clawd is pixel art with hard binary alpha

# 24 个标准固件槽位 -> cc-clawd 动画名（顺序严格对齐 src/anim/nva.h 0..23）
CLAWD_MAP = {
    "idle":         "idle",
    "thinking":     "thinking",
    "working":      "todo",
    "waiting":      "proceed",
    "success":      "passed_long",
    "error":        "error",
    "click":        "hello",
    "sleep":        "sleep",
    "quota":        "lgtm",
    "click_happy":  "right",
    "click_shy":    "duck",
    "click_angry":  "rmrf",
    "click_giggle": "detached_long",
    "crab_walk":    "roam",
    "happy_run":    "roam",
    "coding":       "simple",
    "archive":      "compact",
    "stretch":      "offwork",
    "yawn":         "overtime",
    "wake_up":      "wake",
    "hum":          "coffee",
    "eat_token":    "overflow",
    "bubble":       "fish",
    "quota_worry":  "deadline",
}


def load_sheet_frames(src_dir: str, anim_cfg: dict, canvas_w: int, canvas_h: int) -> list[np.ndarray]:
    """切分精灵表并按 box.x/box.y 复原到完整 canvas_w x canvas_h RGBA 帧数组（按唯一帧下标缓存）。"""
    sheet_path = os.path.join(src_dir, anim_cfg["sheet"])
    im = Image.open(sheet_path).convert("RGBA")
    arr = np.array(im)
    fw = int(anim_cfg["frameW"])
    fh = int(anim_cfg["frameH"])
    cols = int(anim_cfg["cols"])
    bx = int(anim_cfg["box"]["x"])
    by = int(anim_cfg["box"]["y"])

    unique_indices = sorted({int(f["i"]) for f in anim_cfg["frames"]})
    by_idx = {}
    for fi in unique_indices:
        row, col = fi // cols, fi % cols
        sub = arr[row * fh : (row + 1) * fh, col * fw : (col + 1) * fw]
        stage = np.zeros((canvas_h, canvas_w, 4), dtype=np.uint8)
        x1 = min(canvas_w, bx + sub.shape[1])
        y1 = min(canvas_h, by + sub.shape[0])
        stage[by:y1, bx:x1] = sub[: y1 - by, : x1 - bx]
        by_idx[fi] = stage
    return by_idx


def resample_timeline(frames_cfg: list[dict], by_idx: dict[int, np.ndarray], fps: int) -> list[np.ndarray]:
    """把逐帧 ms 时长重采样为固定 fps 帧序列。"""
    step_ms = 1000.0 / float(fps)
    total_ms = sum(int(f["ms"]) for f in frames_cfg)
    n_out = max(1, int(round(total_ms / step_ms)))

    cum_ends = []
    acc = 0
    for f in frames_cfg:
        acc += int(f["ms"])
        cum_ends.append((acc, int(f["i"])))

    out = []
    cur = 0
    for k in range(n_out):
        t = (k + 0.5) * step_ms
        while cur + 1 < len(cum_ends) and t >= cum_ends[cur][0]:
            cur += 1
        out.append(by_idx[cum_ends[cur][1]])
    return out


def crop_anchor_aligned(
    stages: list[np.ndarray],
    anchor_x: int,
    stage_bottom: int,
) -> tuple[list[np.ndarray], tuple[int, int, int, int]]:
    """以 anchor_x 对称裁边，底边对齐 stage_bottom，保证角色中心和脚底跨动作零偏移。"""
    ch, cw = stages[0].shape[:2]
    x0, y0, x1, y1 = cw, ch, 0, 0
    for st in stages:
        ys, xs = np.where(st[:, :, 3] >= ALPHA_CUTOFF)
        if len(xs) == 0:
            continue
        x0 = min(x0, int(xs.min()))
        y0 = min(y0, int(ys.min()))
        x1 = max(x1, int(xs.max()) + 1)
        y1 = max(y1, int(ys.max()) + 1)
    if x1 <= x0:
        raise RuntimeError("empty sprite animation")

    half_w = max(anchor_x - x0, x1 - anchor_x, 1)
    cx0 = max(0, anchor_x - half_w)
    cx1 = min(cw, anchor_x + half_w)
    # 保持严格对称，防止一侧触边导致中心偏移
    sym_half = min(anchor_x - cx0, cx1 - anchor_x)
    if anchor_x - x0 > sym_half or x1 - anchor_x > sym_half:
        # 若另一侧顶到画布边界，尽量扩大到另一侧也够宽，不足则用画布全宽
        cx0 = 0
        cx1 = cw

    cy0 = max(0, y0)
    cy1 = max(stage_bottom, y1)
    cy1 = min(ch, cy1)

    cropped = [st[cy0:cy1, cx0:cx1] for st in stages]
    return cropped, (cx0, cy0, cx1, cy1)


def scale_nearest(crops: list[np.ndarray], scale: float) -> tuple[list[np.ndarray], int, int]:
    """最近邻缩放，且宽高保证为偶数（兼容硬件偶数窗口习惯）。"""
    sh, sw = crops[0].shape[:2]
    dw = max(2, int(round(sw * scale)))
    dh = max(2, int(round(sh * scale)))
    if abs(scale - 1.0) < 1e-4:
        dw, dh = sw, sh
    if dw * dh > MAX_FRAME_PIXELS:
        raise RuntimeError(f"frame {dw}x{dh}={dw*dh} exceeds MAX_FRAME_PIXELS={MAX_FRAME_PIXELS}")
    out = []
    for c in crops:
        if dw == sw and dh == sh:
            out.append(c.copy())
        else:
            im = Image.fromarray(c, mode="RGBA").resize((dw, dh), Image.Resampling.NEAREST)
            out.append(np.array(im, dtype=np.uint8))
    return out, dw, dh


def index_exact_or_quantize(crops: list[np.ndarray], max_colors: int = 255) -> tuple[list[np.ndarray], np.ndarray]:
    """直接提取全部非透明精确 RGB 调色板；超过 max_colors 时再 MEDIANCUT 量化。"""
    stack = np.stack(crops, axis=0)
    vis = stack[:, :, :, 3] >= ALPHA_CUTOFF
    opaque_rgb = stack[:, :, :, :3][vis]
    if len(opaque_rgb) == 0:
        return [np.zeros(c.shape[:2], dtype=np.uint8) for c in crops], np.zeros((1, 3), dtype=np.uint8)

    # 打包成 u32 精确去重，保序稳定
    packed = (
        (opaque_rgb[:, 0].astype(np.uint32) << 16)
        | (opaque_rgb[:, 1].astype(np.uint32) << 8)
        | opaque_rgb[:, 2].astype(np.uint32)
    )
    uniq, inv = np.unique(packed, return_inverse=True)

    if len(uniq) <= max_colors:
        palette = np.stack(
            [(uniq >> 16) & 0xFF, (uniq >> 8) & 0xFF, uniq & 0xFF], axis=-1
        ).astype(np.uint8)
        frames = []
        offset = 0
        for c in crops:
            m = c[:, :, 3] >= ALPHA_CUTOFF
            cnt = int(m.sum())
            idx = np.zeros(c.shape[:2], dtype=np.uint8)
            idx[m] = (inv[offset : offset + cnt] + 1).astype(np.uint8)
            offset += cnt
            frames.append(idx)
        return frames, palette

    # Fallback: quantize if a sprite exceeds 255 unique RGB colors
    q = Image.fromarray(opaque_rgb.reshape(-1, 1, 3), mode="RGB").quantize(
        colors=max_colors, method=Image.Quantize.MEDIANCUT
    )
    palette = np.array(q.getpalette()[: max_colors * 3], dtype=np.uint8).reshape(-1, 3)
    pal_i32 = palette.astype(np.int32)
    frames = []
    for c in crops:
        rgb = c[:, :, :3].astype(np.int32)
        d = ((rgb[:, :, None, :] - pal_i32[None, None, :, :]) ** 2).sum(-1)
        idx = np.where(c[:, :, 3] >= ALPHA_CUTOFF, d.argmin(-1).astype(np.uint8) + 1, 0).astype(np.uint8)
        frames.append(idx)
    return frames, palette


def encode_pack_nva2(
    slot_name: str,
    frames: list[np.ndarray],
    palette: np.ndarray,
    fps: int,
    out_dir: str,
) -> tuple[str, int, int, int, int]:
    """写出标准 NVA2 包（格式与 convert_pet.py / src/anim/nva.cpp 完全一致）。"""
    h, w = frames[0].shape[:2]
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

    path = os.path.join(out_dir, slot_name + ".nva")
    with open(path, "wb") as f:
        f.write(header)
        for b in blobs:
            f.write(b)
    return path, w, h, len(frames), os.path.getsize(path)


def render_frame_rgb(frame: np.ndarray, palette: np.ndarray) -> np.ndarray:
    h, w = frame.shape
    comp = np.zeros((h, w, 3), dtype=np.uint8)
    m = frame > 0
    comp[m] = palette[frame[m] - 1]
    return comp


def build_grid_preview(samples: list[dict], out_path: str) -> None:
    """生成 24 动作总览图（4 列 × 6 行），画脚底基准线和中心线方便人眼检查对齐。"""
    cols = 4
    rows = (len(samples) + cols - 1) // cols
    cell_w, cell_h = 240, 250
    grid = Image.new("RGB", (cols * cell_w, rows * cell_h), (10, 26, 54))  # TIDE 深蓝底方便看对比度
    draw = ImageDraw.Draw(grid)

    for idx, item in enumerate(samples):
        r, c = idx // cols, idx % cols
        x0, y0 = c * cell_w, r * cell_h
        # Cell border and baseline guide
        draw.rectangle([x0 + 2, y0 + 2, x0 + cell_w - 3, y0 + cell_h - 3], outline=(32, 60, 102))
        base_y = y0 + cell_h - 14
        cx = x0 + cell_w // 2
        draw.line([(x0 + 16, base_y), (x0 + cell_w - 16, base_y)], fill=(64, 110, 160), width=1)
        draw.line([(cx, y0 + 26), (cx, base_y)], fill=(35, 65, 110), width=1)

        rgb = item["thumb"]
        th, tw = rgb.shape[:2]
        im = Image.fromarray(rgb, mode="RGB")
        # Paste with bottom-center aligned at (cx, base_y)
        px = cx - tw // 2
        py = base_y - th
        # Replace pure black background with TIDE cell background only where mask==0
        mask = Image.fromarray((item["mask"] * 255).astype(np.uint8), mode="L")
        grid.paste(im, (px, py), mask)

        label = f"{idx:02d} {item['slot']} ({item['src']})\n{item['w']}x{item['h']} {item['kb']}KB"
        draw.text((x0 + 8, y0 + 6), label, fill=(220, 235, 255))

    grid.save(out_path)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="解压后的克码人生 Clawd 目录（含 pack.json）")
    ap.add_argument("--out", required=True, help="输出 .nva 目录")
    ap.add_argument("--scale", type=float, default=DEFAULT_SCALE, help="缩放倍数（默认 1.5）")
    ap.add_argument("--fps", type=int, default=DEFAULT_FPS, help="输出帧率（默认 10）")
    ap.add_argument("--preview", action="store_true", help="导出预览条与 24 动作总览拼图")
    ap.add_argument("--only", default="", help="逗号分隔，只转指定固件槽位")
    args = ap.parse_args()

    pack_path = os.path.join(args.src, "pack.json")
    with open(pack_path, "r", encoding="utf-8") as f:
        pack = json.load(f)

    canvas_w = int(pack["canvas"]["w"])
    canvas_h = int(pack["canvas"]["h"])
    anchor_x = int(pack["anchor"]["x"])
    # 统一贴到底边 144（比 baseline 138 多 6px，兼容 rmrf/passed_long/overtime 下探动作不裁脚）
    stage_bottom = canvas_h

    os.makedirs(args.out, exist_ok=True)
    slots = [s.strip() for s in args.only.split(",") if s.strip()] or list(CLAWD_MAP.keys())

    results = []
    preview_samples = []
    for slot in slots:
        if slot not in CLAWD_MAP:
            print(f"ERROR: unknown slot {slot}", file=sys.stderr)
            sys.exit(1)
        src_anim = CLAWD_MAP[slot]
        anim_cfg = pack["animations"][src_anim]

        by_idx = load_sheet_frames(args.src, anim_cfg, canvas_w, canvas_h)
        stages = resample_timeline(anim_cfg["frames"], by_idx, args.fps)
        crops, crop_box = crop_anchor_aligned(stages, anchor_x, stage_bottom)
        scaled, w, h = scale_nearest(crops, args.scale)
        frames, palette = index_exact_or_quantize(scaled)
        _, w, h, n, size = encode_pack_nva2(slot, frames, palette, args.fps, args.out)

        kb = round(size / 1024.0, 1)
        print(
            f"{slot:13s} <- {src_anim:13s}  {w:3d}x{h:<3d}  {n:3d}帧  "
            f"{len(palette):3d}色  {kb:6.1f}KB"
        )
        results.append({
            "slot": slot,
            "source_anim": src_anim,
            "w": w,
            "h": h,
            "frames": n,
            "fps": args.fps,
            "colors": int(len(palette)),
            "bytes": size,
            "kb": kb,
            "crop_box": list(crop_box),
        })

        if args.preview:
            step = max(1, n // 6)
            picks = [i for i in range(0, n, step)][:6]
            sheet = Image.new("RGB", (w * len(picks), h), (0, 0, 0))
            for s_idx, fi in enumerate(picks):
                sheet.paste(Image.fromarray(render_frame_rgb(frames[fi], palette)), (s_idx * w, 0))
            sheet.save(os.path.join(args.out, f"{slot}_preview.png"))

            # Pick a representative non-blank middle/active frame for contact sheet
            pick_idx = min(n - 1, n // 2 if n > 4 else 0)
            preview_samples.append({
                "slot": slot,
                "src": src_anim,
                "w": w,
                "h": h,
                "kb": kb,
                "thumb": render_frame_rgb(frames[pick_idx], palette),
                "mask": (frames[pick_idx] > 0).astype(np.uint8),
            })

    if args.preview and preview_samples:
        build_grid_preview(preview_samples, os.path.join(args.out, "clawd_preview_grid.png"))

    total = sum(r["bytes"] for r in results)
    manifest = {
        "format": "NVA2",
        "id": pack.get("id", "cc-clawd"),
        "name": pack.get("name", "克码人生 · Clawd"),
        "author": pack.get("author", "alwinecor"),
        "license": pack.get("license", "MIT"),
        "source": pack.get("source", "https://github.com/alwinecor/clawd-stickers"),
        "params": {
            "scale": args.scale,
            "resample": "NEAREST",
            "fps": args.fps,
            "anchor": {"x": anchor_x, "stage_bottom": stage_bottom},
            "xor_delta": True,
            "keyframe_interval": KEYFRAME_INTERVAL,
        },
        "anims": results,
        "total_bytes": total,
        "total_kb": round(total / 1024.0, 1),
    }
    with open(os.path.join(args.out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)

    print(f"\n合计 {len(results)} 个动画 / {total:,} 字节 ({total / 1024.0:.1f} KB / {total / (1024*1024):.2f} MiB)")


if __name__ == "__main__":
    main()
