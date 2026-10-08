#!/usr/bin/env python3
"""
CGPlay EXR/图像序列导出工具
使用 OpenImageIO (OIIO) + OpenCV 实现可靠的 EXR 序列导出

安装依赖:
    pip install OpenImageIO opencv-python numpy

用法:
    python export_video.py <输入路径> <输出视频> [--fps 24] [--start 0] [--end 100]

示例:
    python export_video.py render.0001.exr output.mp4 --fps 24 --start 0 --end 100
    python export_video.py "C:\shots\render.0001.exr" output.mp4
"""

import sys
import os
import re
import glob
import argparse
from pathlib import Path

try:
    import OpenImageIO as oiio
    import cv2
    import numpy as np
except ImportError as e:
    print(f"错误: 缺少依赖 - {e}")
    print("请安装: pip install OpenImageIO opencv-python numpy")
    sys.exit(1)


def parse_sequence_path(path):
    """解析图像序列路径，如 render.0001.exr -> (目录, 基础名, 扩展名, 起始帧)"""
    p = Path(path)
    directory = p.parent
    name = p.stem  # 如 "render.0001"
    ext = p.suffix  # 如 ".exr"
    
    # 匹配帧号模式: name.0001, name_0001, name-0001
    match = re.match(r'^(.*?)[._-](\d+)$', name)
    if match:
        base_name = match.group(1)  # "render"
        frame_num = int(match.group(2))  # 1
        padding = len(match.group(2))  # 4
        return directory, base_name, ext, frame_num, padding
    
    # 无帧号的单帧
    return directory, name, ext, None, 0


def find_sequence_frames(directory, base_name, ext, start_frame, end_frame, padding):
    """查找序列中的所有帧文件"""
    frames = []
    pattern = f"{base_name}.%0{padding}d{ext}"
    
    for frame in range(start_frame, end_frame + 1):
        filename = pattern % frame
        filepath = directory / filename
        if filepath.exists():
            frames.append(filepath)
        else:
            print(f"警告: 缺失帧 {frame}: {filepath}")
    
    return frames


def read_exr_with_oiio(filepath):
    """使用 OIIO 读取 EXR，自动处理色彩空间"""
    img = oiio.ImageInput.open(str(filepath))
    if not img:
        print(f"错误: 无法读取 {filepath}")
        return None
    
    spec = img.spec()
    data = img.read_image(oiio.FLOAT)
    img.close()
    
    # 转换为 OpenCV 格式 (BGR, uint8)
    # OIIO 返回的是 RGBA float，范围通常是 [0,1] 或更高（线性）
    
    # 分离通道
    if data.shape[2] >= 3:
        r = data[:, :, 0]
        g = data[:, :, 1] 
        b = data[:, :, 2]
    else:
        r = g = b = data[:, :, 0]
    
    # 线性到 sRGB 转换（gamma 2.2）
    def linear_to_srgb(x):
        # 更精确的 sRGB 转换
        x = np.clip(x, 0, None)
        # 对于小值用线性部分，大值用 gamma
        mask = x <= 0.0031308
        result = np.where(mask, x * 12.92, 1.055 * np.power(x, 1/2.4) - 0.055)
        return np.clip(result, 0, 1)
    
    r = linear_to_srgb(r)
    g = linear_to_srgb(g)
    b = linear_to_srgb(b)
    
    # 合并为 BGR (OpenCV 格式)
    bgr = np.stack([b, g, r], axis=2)
    
    # 转为 uint8
    bgr_uint8 = (bgr * 255).astype(np.uint8)
    
    return bgr_uint8


def export_video(input_path, output_path, fps=24, start_frame=0, end_frame=None):
    """主导出函数"""
    directory, base_name, ext, frame_num, padding = parse_sequence_path(input_path)
    
    print(f"解析路径: dir={directory}, base={base_name}, ext={ext}")
    print(f"帧号: {frame_num}, padding={padding}")
    
    if frame_num is None:
        print("错误: 无法解析帧号。请确保文件名格式为 name.0001.ext")
        return False
    
    # 自动检测帧范围
    if end_frame is None:
        # 查找所有匹配的文件
        pattern = str(directory / f"{base_name}.*{ext}")
        files = glob.glob(pattern)
        if not files:
            print(f"错误: 未找到匹配的文件: {pattern}")
            return False
        
        # 提取所有帧号
        frame_numbers = []
        for f in files:
            m = re.search(r'[._-](\d+)' + re.escape(ext) + '$', f)
            if m:
                frame_numbers.append(int(m.group(1)))
        
        if not frame_numbers:
            print("错误: 无法从文件列表提取帧号")
            return False
        
        start_frame = min(frame_numbers)
        end_frame = max(frame_numbers)
        print(f"自动检测帧范围: {start_frame} - {end_frame}")
    
    # 查找所有帧
    frames = find_sequence_frames(directory, base_name, ext, start_frame, end_frame, padding)
    if not frames:
        print("错误: 未找到任何帧文件")
        return False
    
    print(f"找到 {len(frames)} 帧")
    
    # 读取第一帧获取尺寸
    first_frame = read_exr_with_oiio(frames[0])
    if first_frame is None:
        return False
    
    height, width = first_frame.shape[:2]
    print(f"分辨率: {width}x{height}")
    
    # 创建视频编码器
    fourcc = cv2.VideoWriter_fourcc(*'mp4v')  # MP4
    out = cv2.VideoWriter(output_path, fourcc, fps, (width, height))
    
    if not out.isOpened():
        print(f"错误: 无法创建视频文件: {output_path}")
        return False
    
    # 写入所有帧
    for i, frame_path in enumerate(frames):
        print(f"处理帧 {i+1}/{len(frames)}: {frame_path.name}", end='\r')
        
        frame = read_exr_with_oiio(frame_path)
        if frame is not None:
            out.write(frame)
    
    print()  # 换行
    out.release()
    print(f"导出完成: {output_path}")
    return True


def main():
    parser = argparse.ArgumentParser(description='CGPlay EXR 序列导出工具')
    parser.add_argument('input', help='输入文件路径 (如 render.0001.exr)')
    parser.add_argument('output', help='输出视频路径 (如 output.mp4)')
    parser.add_argument('--fps', type=float, default=24, help='帧率 (默认: 24)')
    parser.add_argument('--start', type=int, default=None, help='起始帧')
    parser.add_argument('--end', type=int, default=None, help='结束帧')
    
    args = parser.parse_args()
    
    success = export_video(
        args.input, 
        args.output, 
        fps=args.fps,
        start_frame=args.start if args.start else 0,
        end_frame=args.end
    )
    
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
