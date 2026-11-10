"""Deploy a native probe's Qt and compiler DLLs from its configured SDK."""
from pathlib import Path
import shutil


def deploy_runtime(executable, qt_root, native_directories=()):
    destination = Path(executable).parent
    # SDK selection must win over a differently versioned packaged Qt. Only
    # non-Qt native dependencies may be taken from the product runtime.
    for directory in native_directories:
        for dll in Path(directory).glob("*.dll"):
            if not dll.name.lower().startswith("qt6"):
                shutil.copy2(dll, destination / dll.name)
    for name in ("Core", "Gui", "Widgets", "Concurrent", "Network", "OpenGL", "OpenGLWidgets"):
        dll = Path(qt_root) / "bin" / ("Qt6" + name + ".dll")
        if dll.exists():
            shutil.copy2(dll, destination / dll.name)
