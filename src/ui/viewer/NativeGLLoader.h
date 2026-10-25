#pragma once

namespace cgplay {

using NativeGLProc = void (*)();
using NativeGLProcLoader = NativeGLProc (*)(const char*);

// Keep the native loader's GL declarations out of Qt's OpenGL headers.
bool initializeNativeGL(NativeGLProcLoader);

} // namespace cgplay
