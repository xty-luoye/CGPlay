# Qualified CGPlay image/color headers must precede general legacy SDK includes.
foreach(_cgplay_prefix IN LISTS CGPLAY_NATIVE_HEADER_PREFIXES)
    if(NOT IS_DIRECTORY "${_cgplay_prefix}/include")
        message(FATAL_ERROR "Qualified native include directory is missing: ${_cgplay_prefix}")
    endif()
    include_directories(BEFORE "${_cgplay_prefix}/include")
endforeach()
