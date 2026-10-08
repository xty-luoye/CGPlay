# Minimal ftkConfig.cmake - ftk targets provided via add_subdirectory
# Creates ALIAS targets so tlRender can use ftk:: namespaced targets

function(_ftk_alias_lib NAMESPACE_TARGET REAL_TARGET)
    if(TARGET ${REAL_TARGET} AND NOT TARGET ${NAMESPACE_TARGET})
        get_target_property(_type ${REAL_TARGET} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            add_executable(${NAMESPACE_TARGET} ALIAS ${REAL_TARGET})
        else()
            add_library(${NAMESPACE_TARGET} ALIAS ${REAL_TARGET})
        endif()
    endif()
endfunction()

_ftk_alias_lib(ftk::ftkCore  ftkCore)
_ftk_alias_lib(ftk::ftkGL    ftkGL)
_ftk_alias_lib(ftk::ftkUI    ftkUI)

# Optional targets
_ftk_alias_lib(ftk::ftkResource   ftkResource)
_ftk_alias_lib(ftk::ftkTestLib    ftkTestLib)
_ftk_alias_lib(ftk::ftkGLTest     ftkGLTest)
_ftk_alias_lib(ftk::ftkCoreTest   ftkCoreTest)
_ftk_alias_lib(ftk::ftkUITest     ftkUITest)
_ftk_alias_lib(ftk::ftkCorePy     ftkCorePy)
_ftk_alias_lib(ftk::ftkUIPy       ftkUIPy)
_ftk_alias_lib(ftk::ftk-resource  ftk-resource)

set(ftk_FOUND TRUE)
