#!/usr/bin/env python3
"""
CGPlay EXR 导出 - 简易版（只需要 imageio + numpy + opencv-python）
支持 EXR 序列转 MP4，自动线性→sRGB 转换

安装:
    pip install imageio imageio-ffmpeg numpy opencv-python tifffile

用法:
    python export_video_simple.py render.0001.exr output.mp4 --fps 24
    python export_video_simple.py C:\shots\*.exr output.mp4
"""

import sys
import os
import re
import glob
import argparse
import math

try:
    import numpy as np
    import cv2
    import imageio.v3 as iio
except ImportError as e:
    print(f"缺少依赖: {e}")
    print("安装: pip install imageio numpy opencv-python")
    sys.exit(1)


def read_exr(filepath):
    """读取 EXR，返回 RGB uint8 数组 (已做线性→sRGB)"""
    try:
        # imageio 读 EXR → float32 HWC
        img = iio.imread(filepath)
        if img.dtype != np.float32:
            img = img.astype(np.float32)
        
        if len(img.shape) == 2:
            img = np.stack([img] * 3, axis=2)
        elif img.shape[2] > 3:
            img = img[:, :, :3]
        
        # 线性 → sRGB (精确公式)
        x = np.clip(img, 0, None)
        mask = x <= 0.0031308
        result = np.where(mask, x * 12.92, 1.055 * np.power(x, 1.0/2.4) - 0.055)
        result = np.clip(result, 0, 1)
        
        # RGB → BGR (OpenCV) + uint8
        bgr = np.stack([result[:,:,2], result[:,:,1], result[:,:,0]], axis=2)
        return (bgr * 255).astype(np.uint8)
    except Exception as e:
        print(f"读取失败 {filepath}: {e}")
        return None


def build_frames(path):
    """从路径构建帧序列"""
    directory = os.path.dirname(path) or '.'
    basename = os.path.basename(path)
    
    # 匹配 name.NNNN.ext 模式
    m = re.match(r'^(.+?)[._-](\d+)\.(.+)$', basename)
    if not m:
        return None, None, None, None
    
    base = m.group(1)
    padding = len(m.group(2))
    ext = m.group(3)
    first_num = int(m.group(2))
    
    pattern = f"{base}.%0{padding}d.{ext}"
    
    # 寻找目录下所有匹配的文件
    frames = {}
    for f in os.listdir(directory):
        m2 = re.match(rf'^{re.escape(base)}\.(\d+)\.{re.escape(ext)}$', f)
        if m2:
            num = int(m2.group(1))
            frames[num] = os.path.join(directory, f)
    
    if not frames:
        return None, None, None, None
    
    nums = sorted(frames.keys())
    return frames, nums[0], nums[-1], pattern


def main():
    parser = argparse.ArgumentParser(description='EXR序列转MP4')
    parser.add_argument('input', help='输入文件 (序列的第一个文件)')
    parser.add_argument('output', help='输出视频 (如 output.mp4)')
    parser.add_argument('--fps', type=float, default=24, help='帧率')
    parser.add_argument('--start', type=int, default=None)
    parser.add_argument('--end', type=int, default=None)
    parser.add_argument('--gamma', type=float, default=2.4, help='Gamma值 (默认2.4=sRGB)')
    args = parser.parse_args()
    
    frames, first, last, pattern = build_frames(args.input)
    
    if not frames:
        print("错误: 未找到图像序列。请确保文件名格式为 name.0001.ext")
        return 1
    
    start = args.start if args.start is not None else first
    end = args.end if args.end is not None else last
    
    print(f"找到 {len(frames)} 帧 (帧 {first}-{last})")
    print(f"导出范围: {start}-{end}")
    
    # 读首帧获取尺寸
    first_img = read_exr(frames[start])
    if first_img is None:
        print("错误: 无法读取第一帧")
        return 1
    
    h, w = first_img.shape[:2]
    print(f"分辨率: {w}x{h}")
    
    # 写视频
    fourcc = cv2.VideoWriter_fourcc(*'avc1')
    writer = cv2.VideoWriter(args.output, fourcc, args.fps, (w, h))
    
    for f in range(start, end + 1):
        filepath = frames.get(f)
        if not filepath:
            print(f"跳过缺失帧 {f}")
            continue
        
        print(f"处理帧 {f} ({f-start+1}/{end-start+1})\r", end='')
        img = read_exr(filepath)
        if img is not None:
            writer.write(img)
    
    writer.release()
    print(f"\n导出完成: {args.output}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
