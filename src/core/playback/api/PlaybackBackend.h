#pragma once

#include <memory>

#if __has_include(<tlRender/Timeline/Player.h>)
  #include <tlRender/Timeline/Player.h>
  #ifndef CGPLAY_HAS_TLRENDER
    #define CGPLAY_HAS_TLRENDER 1
  #endif
#else
  #ifndef CGPLAY_HAS_TLRENDER
    #ifdef TL_STATIC
      #define CGPLAY_HAS_TLRENDER 1
    #else
      #define CGPLAY_HAS_TLRENDER 0
    #endif
  #endif
#endif

#if __has_include(<ftk/Core/Context.h>)
  #include <ftk/Core/Context.h>
  #include <ftk/UI/Style.h>
  #ifndef CGPLAY_HAS_FTK
    #define CGPLAY_HAS_FTK 1
  #endif
#else
  #ifndef CGPLAY_HAS_FTK
    #ifdef TL_STATIC
      #define CGPLAY_HAS_FTK 1
    #else
      #define CGPLAY_HAS_FTK 0
    #endif
  #endif
#endif
