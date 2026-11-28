#!/usr/bin/env python3
"""把指定宠物目录（默认 assets/pets/clawd）通过 USB 串口写入设备端 TF 卡 `/sdcard/pets/<pet>/`。

前置条件：设备已插好可挂载的 FAT32 TF 卡（`{"cmd":"sd"}` 自检 mounted=true）。

## 为什么原来会超时

设备走 USB-Serial/JTAG 硬件 CDC，Arduino 核心给它的接收队列默认只有 **256 字节**
（`HWCDC.cpp` / `USBCDC.cpp` 的 `setRxBufferSize(256)`）。固件 `pollSerialCommands()`
在主循环里逐字节取，主循环被画屏或 SD 写入占住时队列就满；队列满时 ISR 直接丢掉
该 USB 包剩下的字节，一行 JSON 从中间截断，`deserializeJson` 失败，命令静默消失。

`sdput` 一行的 base64 会把原始字节膨胀到 4/3，历史默认 `--chunk 640` 产生约 934 字节
的行，**必然**被截断，于是等 6 秒超时——卡没坏，是串口接收队列装不下这一行。

## 现在的做法

1. **分片喂料**：一行不一次性 write，拆成 `--piece`（默认 120 字节）小片、每片间隔
   `--gap-ms`（默认 22ms）。每片都在 256 字节队列容量内，主循环有足够机会把它取走。
   实测 100 块零失败（`chunk=640 / piece=120 / gap=22ms`）。
2. **失败整文件重来**：`append=true` 用的是 `SD_MMC.open(path, FILE_APPEND)`，只重发
   单个失败块会把那 640 字节再追加一遍，文件变成「正确内容 + 重复片段」，`parsePack()`
   直接判非法。所以任何一块失败就回到 off=0（`append=false` 会截断重写）。
3. **写完整角色切换做校验**：`{"cmd":"pet"}` 会真的加载 idle/success/error 三个包，
   `ok=true` 才说明落盘内容逐字节可解析。

全量 24 个文件 / 203 KB 实测约 1.5 分钟。

## 想更快就改固件

在 `src/hw/hw.cpp` 的 `hwInit()` 里，`Serial.begin(115200)` 之前加一行
`Serial.setRxBufferSize(4096);`，之后 `--piece 0`（不分片整行发）可用，一次写完不到十秒。
"""

import argparse
import base64
import glob
import json
import os
import sys
import time

try:
    import serial
except ImportError:
    print("需要 pyserial（可用项目内 .venv-shot/bin/python 运行）", file=sys.stderr)
    sys.exit(2)


class Device:
    def __init__(self, port: str, settle_s: float = 1.0) -> None:
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.25
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        time.sleep(settle_s)
        self.ser.reset_input_buffer()

    def close(self) -> None:
        self.ser.close()

    def write_line(self, payload: dict, piece: int, gap_ms: float) -> None:
        line = (json.dumps(payload) + "\n").encode("utf-8")
        if piece <= 0 or piece >= len(line):
            self.ser.write(line)
            self.ser.flush()
            return
        gap = gap_ms / 1000.0
        for i in range(0, len(line), piece):
            self.ser.write(line[i : i + piece])
            self.ser.flush()
            if gap:
                time.sleep(gap)

    def ask(self, payload: dict, key: str, timeout_s: float = 6.0,
            piece: int = 120, gap_ms: float = 22.0) -> dict:
        self.write_line(payload, piece, gap_ms)
        prefix = f'{{"{key}":'
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            line = self.ser.readline().decode("utf-8", "replace").strip()
            if line.startswith(prefix):
                return json.loads(line)
        raise TimeoutError(key)

    def resync(self, rounds: int = 3) -> None:
        """灌空行，把设备侧残留的半行冲掉（空行只让行缓冲归零）。"""
        self.ser.reset_input_buffer()
        for _ in range(rounds):
            self.ser.write(b"\n")
            self.ser.flush()
            time.sleep(0.03)
        self.ser.reset_input_buffer()


def put_file(dev: Device, rel_path: str, data: bytes, chunk: int, piece: int,
             gap_ms: float, attempts: int) -> bool:
    """整文件写入；任一块失败就回到 off=0 截断重写。"""
    for attempt in range(1, attempts + 1):
        failed_off = None
        for off in range(0, len(data), chunk):
            part = data[off : off + chunk]
            payload = {
                "cmd": "sdput",
                "path": rel_path,
                "append": off > 0,
                "b64": base64.b64encode(part).decode("ascii"),
            }
            try:
                res = dev.ask(payload, "sdput", timeout_s=3.0, piece=piece,
                              gap_ms=gap_ms).get("sdput", {})
            except TimeoutError:
                res = {}
            if not res.get("ok") or res.get("bytes") != len(part):
                failed_off = off
                break
        if failed_off is None:
            return True
        print(f"  {rel_path} @{failed_off} 掉块，第 {attempt}/{attempts} 次整文件重写",
              file=sys.stderr)
        dev.resync()
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description="Stage pet .nva assets onto device TF card via serial")
    ap.add_argument("--port", default="/dev/cu.usbmodem21101")
    ap.add_argument("--pet", default="clawd")
    ap.add_argument("--src", default="")
    ap.add_argument("--chunk", type=int, default=640, help="每次 sdput 的原始字节数")
    ap.add_argument("--piece", type=int, default=120,
                    help="一行拆成多大的小片发送（0=整行直发；单片须 < 256 字节 CDC 队列）")
    ap.add_argument("--gap-ms", type=float, default=22.0, help="小片之间的间隔毫秒")
    ap.add_argument("--retries", type=int, default=6, help="单文件掉块后整文件重写次数")
    ap.add_argument("--no-switch", action="store_true", help="写完不自动切角色")
    args = ap.parse_args()

    src_dir = args.src or os.path.join("assets", "pets", args.pet)
    nva_files = sorted(glob.glob(os.path.join(src_dir, "*.nva")))
    if not nva_files:
        print(f"找不到 .nva 文件: {src_dir}", file=sys.stderr)
        return 1

    dev = Device(args.port)
    sd = dev.ask({"cmd": "sd"}, "sd", timeout_s=6.0).get("sd", {})
    if not sd.get("mounted"):
        print(f"TF 卡未挂载，无法写入: {json.dumps(sd, ensure_ascii=False)}", file=sys.stderr)
        dev.close()
        return 1
    print(f"TF 卡 OK: {sd.get('type')} {sd.get('cardMB')}MB {sd.get('name')} "
          f"@{sd.get('freqKhz')}KHz，空闲 {sd.get('totalMB', 0) - sd.get('usedMB', 0)}MB")

    rel_dir = f"/pets/{args.pet}"
    mk = dev.ask({"cmd": "sdmkdir", "path": rel_dir}, "sdmkdir").get("sdmkdir", {})
    if not mk.get("ok"):
        print(f"创建目录失败: {mk}", file=sys.stderr)
        dev.close()
        return 1

    total_bytes = sum(os.path.getsize(f) for f in nva_files)
    done_bytes = 0
    failed = []
    t0 = time.time()
    for idx, fpath in enumerate(nva_files, 1):
        fname = os.path.basename(fpath)
        rel_path = f"{rel_dir}/{fname}"
        with open(fpath, "rb") as f:
            data = f.read()
        if not put_file(dev, rel_path, data, args.chunk, args.piece, args.gap_ms,
                        args.retries):
            failed.append(fname)
            print(f"[{idx:2d}/{len(nva_files)}] FAIL {rel_path}", file=sys.stderr)
            dev.resync()
            continue
        done_bytes += len(data)
        elapsed = time.time() - t0
        eta = elapsed / done_bytes * (total_bytes - done_bytes) if done_bytes else 0
        print(f"[{idx:2d}/{len(nva_files)}] OK {fname} ({len(data)}B) "
              f"{elapsed:5.0f}s 已写 {done_bytes}/{total_bytes}B 剩余约 {eta:4.0f}s")
        sys.stdout.flush()

    print(f"\n写入完成 {len(nva_files) - len(failed)}/{len(nva_files)} 个文件，"
          f"{done_bytes} 字节，耗时 {time.time() - t0:.0f}s")

    if failed:
        print(f"失败: {', '.join(failed)}", file=sys.stderr)
        dev.close()
        return 1

    # 切角色会真的加载 idle/success/error 三个包，ok=true 才证明落盘内容能解析。
    if not args.no_switch:
        try:
            pet_res = dev.ask({"cmd": "pet", "pet": args.pet}, "pet", timeout_s=20.0,
                              piece=args.piece, gap_ms=args.gap_ms).get("pet", {})
        except TimeoutError:
            pet_res = {"ok": None, "err": "timeout"}
        print(f"设备切角色: {json.dumps(pet_res, ensure_ascii=False)}")
        if pet_res.get("ok") is not True:
            print("切角色失败：落盘内容没能被设备解析", file=sys.stderr)
            dev.close()
            return 1
    dev.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
