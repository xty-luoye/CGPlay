"""CGPlay 导出 CLI — 被 C++ 调用（无 ffmpeg 依赖）"""

import sys
import os
import re
import numpy as np
import imageio.v3 as iio
import cv2


def apply_tone_map(x):
    """Reinhard 全局色调映射：HDR → [0,1]，保留高光细节"""
    return x / (1.0 + x)


def srgb_transform(img):
    """线性 → sRGB（Reinhard 映射 + sRGB gamma）"""
    x = img.astype(np.float32)
    x = apply_tone_map(x)  # 先压缩 HDR 范围
    mask = x <= 0.0031308
    result = np.where(mask, x * 12.92, 1.055 * np.power(np.maximum(x, 0), 1.0 / 2.4) - 0.055)
    return np.clip(result, 0, 1)


def read_frame(path):
    """读帧 → sRGB BGR uint8"""
    img = iio.imread(path).astype(np.float32)
    if len(img.shape) == 2:
        img = np.stack([img] * 3, axis=2)
    elif img.shape[2] >= 3:
        img = img[:, :, :3]
    img = srgb_transform(img)
    bgr = np.stack([img[:, :, 2], img[:, :, 1], img[:, :, 0]], axis=2)
    return (bgr * 255).astype(np.uint8)


def write_progress(current, total):
    sys.stdout.write(f"PROGRESS {current}/{total}\n")
    sys.stdout.flush()


def cmd_encode(args):
    """编码 PNG 序列 → MP4"""
    pattern = args[1]
    output = args[2]
    fps = float(args[3])
    start = int(args[4])
    count = int(args[5])
    w = int(args[6])
    h = int(args[7])

    writer = cv2.VideoWriter(output, cv2.VideoWriter_fourcc(*'avc1'), fps, (w, h))
    for i in range(count):
        frame_num = start + i
        path = pattern % frame_num
        if not os.path.exists(path):
            sys.stderr.write(f"MISSING {path}\n")
            continue
        img = read_frame(path)
        writer.write(img)
        if i % 5 == 0 or i == count - 1:
            write_progress(i + 1, count)
    writer.release()
    sys.stdout.write("DONE\n")
    sys.stdout.flush()


def cmd_extract(args):
    """EXR → PNG 序列（支持 . _ - 三种分隔符）"""
    input_path = args[1]
    output_dir = args[2]
    start_str = args[4]
    end_str = args[5]

    directory = os.path.dirname(input_path) or '.'
    basename = os.path.basename(input_path)
    m = re.match(r'^(.+?)[._-](\d+)\.(.+)$', basename)
    if not m:
        sys.stderr.write(f"ERROR: cannot parse filename: {basename}\n")
        sys.exit(1)

    base = m.group(1)
    ext = m.group(3)
    start_num = int(start_str) if start_str != 'auto' else None
    end_num = int(end_str) if end_str != 'auto' else None

    # 扫描所有帧（匹配 . _ - 分隔符）
    frames = {}
    for f in os.listdir(directory):
        m2 = re.match(rf'^{re.escape(base)}[._-](\d+)\.{re.escape(ext)}$', f)
        if m2:
            num = int(m2.group(1))
            frames[num] = os.path.join(directory, f)

    if not frames:
        sys.stderr.write(f"ERROR: no frames found in {directory} matching {base}[._-]*.{ext}\n")
        sys.exit(1)

    all_nums = sorted(frames.keys())
    if start_num is None:
        start_num = all_nums[0]
    if end_num is None:
        end_num = all_nums[-1]

    os.makedirs(output_dir, exist_ok=True)
    total = end_num - start_num + 1
    done = 0

    for f in range(start_num, end_num + 1):
        path = frames.get(f)
        if not path:
            sys.stderr.write(f"MISSING frame {f}\n")
            continue
        img = read_frame(path)
        out_name = f"frame_{f:06d}.png"
        cv2.imwrite(os.path.join(output_dir, out_name), img)
        done += 1
        if done % 5 == 0 or done == total:
            write_progress(done, total)

    sys.stdout.write(f"EXTRACTED {done}\n")
    sys.stdout.flush()


def main():
    if len(sys.argv) < 3:
        print("Usage: python export_cli.py extract|encode ...", file=sys.stderr)
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == 'encode':
        cmd_encode(sys.argv)
    elif cmd == 'extract':
        cmd_extract(sys.argv)
    else:
        sys.stderr.write(f"Unknown command: {cmd}\n")
        sys.exit(1)


if __name__ == '__main__':
    main()
