cmake_minimum_required(VERSION 3.21)
project(CGPlayWindowsThumbnailSmoke LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The Windows Shell thumbnail smoke requires Windows")
endif()

add_executable(windows_thumbnail_smoke main.cpp)
target_compile_features(windows_thumbnail_smoke PRIVATE cxx_std_17)
target_compile_definitions(windows_thumbnail_smoke PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(windows_thumbnail_smoke PRIVATE ole32 shell32 uuid gdi32 user32)
if(MSVC)
    target_compile_options(windows_thumbnail_smoke PRIVATE /W4 /utf-8)
endif()
