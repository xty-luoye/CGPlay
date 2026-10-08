"""HDR Tone Mapping — OCIO 色彩管理 (匹配 CGPlay 显示效果)

优先使用 OpenColorIO（与 CGPlay 相同配置），回退到 ACES 胶片近似。
"""

import numpy as np

_HAS_OCIO = False
_OCIO_CPU = None

try:
    import PyOpenColorIO as OCIO
    # Load same builtin config that CGPlay uses
    for cfg_name in [
        'ocio://studio-config-v1.0.0_aces-v1.3_ocio-v2.1',
        'ocio://default',
    ]:
        try:
            _oco_config = OCIO.Config.CreateFromBuiltinConfig(cfg_name)
            break
        except Exception:
            continue
    else:
        raise ImportError("No OCIO builtin config available")

    _oco_display = "sRGB - Display"
    _oco_view = "ACES 1.0 - SDR Video"
    _oco_input = "scene_linear"

    _oco_processor = _oco_config.getProcessor(
        _oco_input, _oco_display, _oco_view, OCIO.TRANSFORM_DIR_FORWARD
    )
    _OCIO_CPU = _oco_processor.getDefaultCPUProcessor()
    _HAS_OCIO = True

except ImportError:
    pass


def apply_pipeline(img: np.ndarray) -> np.ndarray:
    """
    默认管线：OCIO (ACES 1.0 SDR-video) 或 ACES 胶片映射 → sRGB。

    与 CGPlay 的显示效果一致。
    输入: HDR 线性 RGB [0, inf)，输出: sRGB [0, 1]
    """
    if _HAS_OCIO and _OCIO_CPU is not None:
        return _apply_ocio(img)
    else:
        return _apply_aces_fallback(img)


def _apply_ocio(rgb: np.ndarray) -> np.ndarray:
    """使用 OCIO 做精确色彩转换"""
    h, w = rgb.shape[:2]
    # OCIO applyRGB 原地修改，需要 contiguous float32
    flat = np.ascontiguousarray(
        rgb[:, :, :3].reshape(-1, 3).astype(np.float32)
    )
    _OCIO_CPU.applyRGB(flat)
    result = flat.reshape(h, w, 3)

    # Clamp (OCIO 可能产生负值/超过 1.0 的值)
    return np.clip(result, 0.0, 1.0)


def _apply_aces_fallback(img: np.ndarray) -> np.ndarray:
    """回退方案：ACES 胶片近似 + sRGB"""
    return linear_to_srgb(aces_filmic(img))


def aces_filmic(x: np.ndarray) -> np.ndarray:
    """
    ACES 近似胶片色调映射。
    输入: HDR 线性值 [0, inf)，输出: [0, ~1]
    """
    x = np.maximum(x.astype(np.float32), 0.0)
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    return (x * (a * x + b)) / (x * (c * x + d) + e)


def linear_to_srgb(img: np.ndarray) -> np.ndarray:
    """精确 sRGB 传递函数。线性 [0, 1] → sRGB [0, 1]。"""
    x = np.maximum(img.astype(np.float32), 0.0)
    mask = x <= 0.0031308
    result = np.where(
        mask,
        x * 12.92,
        1.055 * np.power(x, 1.0 / 2.4) - 0.055,
    )
    return np.clip(result, 0.0, 1.0)
