#include "NativeGLLoader.h"

// Define the version before selecting the native context loader.
#if __has_include(<tlRender/Core/Version.h>)
#include <tlRender/Core/Version.h>
#endif
#include <ftk/GL/GL.h>
#include <ftk/GL/Init.h>

namespace cgplay {

bool initializeNativeGL(NativeGLProcLoader load)
{
#if TLRENDER_VERSION_MAJOR > 0 || TLRENDER_VERSION_MINOR >= 27
    ftk::gl::setAPI(ftk::gl::API::GL_4_1);
    return load && gladLoadGL(load) != 0;
#else
    ftk::gl::initGLAD();
    return true;
#endif
}

} // namespace cgplay
