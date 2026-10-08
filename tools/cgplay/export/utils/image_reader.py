"""ImageReader — 读 EXR/PNG/JPG/TIFF，返回 RGB uint8

优先使用 OpenImageIO，回退到 imageio + OpenCV。
"""

import os
import re
import sys
import numpy as np
from typing import Dict, Tuple, Optional, List

from .tone_mapping import apply_pipeline

# 尝试导入 OIIO，不可用时回退 imageio
_HAS_OIIO = False
try:
    import OpenImageIO as oiio
    _HAS_OIIO = True
except ImportError:
    pass


class ImageReader:
    """使用 OpenImageIO（优先）或 imageio 读取图像序列"""

    def __init__(self):
        pass

    @staticmethod
    def read_frame(path: str) -> np.ndarray:
        """
        读单帧 → RGB uint8（已做 Reinhard + sRGB）
        返回形状 (H, W, 3) numpy 数组。
        """
        if _HAS_OIIO:
            try:
                return _read_oiio(path)
            except Exception as e:
                # OIIO 本身出错（格式不支持等），回退 imageio
                sys.stderr.write(f"[ImageReader] OIIO failed for {path}: {e}\n")
                sys.stderr.flush()

        # 回退到 imageio
        try:
            return _read_imageio(path)
        except ImportError:
            raise ImportError(
                "No image reader available. "
                "Install OpenImageIO: pip install openimageio\n"
                "Or imageio: pip install imageio"
            )

    @staticmethod
    def scan_sequence(input_path: str) -> Dict[int, str]:
        """
        扫描图像序列目录。
        支持 render.0001.exr / render_0001.exr / render-0001.exr
        返回 {frame_number: full_path}
        """
        directory = os.path.dirname(input_path) or '.'
        basename = os.path.basename(input_path)

        m = re.match(r'^(.+?)[._-](\d+)\.(.+)$', basename)
        if not m:
            if os.path.isfile(input_path):
                return {0: input_path}
            raise FileNotFoundError(f"Cannot parse sequence from: {basename}")

        base = m.group(1)
        ext = m.group(3)
        frames = {}

        for f in os.listdir(directory):
            m2 = re.match(rf'^{re.escape(base)}[._-](\d+)\.{re.escape(ext)}$', f)
            if m2:
                num = int(m2.group(1))
                frames[num] = os.path.join(directory, f)

        if not frames:
            raise FileNotFoundError(
                f"No frames found in {directory} matching {base}[._-]*.{ext}"
            )

        return frames

    @staticmethod
    def get_sequence_range(
        frames: Dict[int, str],
        start: Optional[int] = None,
        end: Optional[int] = None
    ) -> Tuple[int, int, List[Tuple[int, str]]]:
        nums = sorted(frames.keys())
        actual_min, actual_max = nums[0], nums[-1]

        # 如果请求范围与实际帧完全不重叠，使用实际范围
        if start is not None and end is not None:
            if end < actual_min or start > actual_max:
                # 请求范围完全在序列之外 — 自动修正为实际范围
                start, end = actual_min, actual_max
        if start is None:
            start = actual_min
        if end is None:
            end = actual_max

        frame_paths = []
        for f in range(start, end + 1):
            p = frames.get(f)
            if p:
                frame_paths.append((f, p))

        # 如果还是没有找到帧（不连续的帧号），收集所有帧
        if not frame_paths:
            frame_paths = [(n, frames[n]) for n in nums if start <= n <= end]
            if not frame_paths:
                frame_paths = [(n, frames[n]) for n in nums]

        return start, end, frame_paths


# ═══════════════════════════════════════════════════════════════════════════════════
# Internal readers
# ═══════════════════════════════════════════════════════════════════════════════════

def _read_oiio(path: str) -> np.ndarray:
    """用 OIIO 读帧"""
    img = oiio.ImageInput.open(path)
    if not img:
        raise IOError(f"OIIO: cannot open {path}")

    data = img.read_image(oiio.FLOAT)
    img.close()

    return _process_pixels(data)


def _read_imageio(path: str) -> np.ndarray:
    """用 imageio 读帧（回退方案）"""
    import imageio.v3 as iio

    data = iio.imread(path)
    return _process_pixels(data)


def _process_pixels(data: np.ndarray) -> np.ndarray:
    """统一处理：通道 → 色调映射 → sRGB → RGB uint8
    
    注：PyAV 需要 BGR uint8 输入，所以此处返回 BGR 顺序。
    """
    h, w = data.shape[:2]

    # 通道处理
    source_dtype = data.dtype
    if len(data.shape) == 2:
        rgb = np.stack([data, data, data], axis=2)
    elif data.shape[2] >= 3:
        rgb = data[:, :, :3]  # RGB order (OIIO returns RGB)
    else:
        raise ValueError(f"Unexpected image shape: {data.shape}")

    if np.issubdtype(source_dtype, np.integer):
        rgb = rgb.astype(np.float32) / np.iinfo(source_dtype).max
    else:
        rgb = rgb.astype(np.float32, copy=False)

    # 色调映射 + sRGB
    rgb = apply_pipeline(rgb)

    # RGB → BGR uint8 (PyAV 需要 BGR)
    bgr = np.stack([rgb[:, :, 2], rgb[:, :, 1], rgb[:, :, 0]], axis=2)
    result = (np.clip(bgr, 0, 1) * 255).astype(np.uint8)
    if data.ndim == 3 and data.shape[2] >= 4:
        alpha = data[:, :, 3]
        if np.issubdtype(source_dtype, np.integer):
            alpha = alpha.astype(np.float32) / np.iinfo(source_dtype).max
        else:
            alpha = alpha.astype(np.float32, copy=False)
        alpha_u8 = (np.clip(alpha, 0, 1) * 255).astype(np.uint8)
        result = np.dstack([result, alpha_u8])
    return result
