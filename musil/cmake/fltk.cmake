# fltk.cmake — FLTK, Musil's one external dependency (plots, controls), fetched and built statically
# as part of the build. Moritz's build uses it too, so both link the same FLTK.
#
#   musil_fetch_fltk()   defines fltk::fltk, fltk::images ... unless a parent build already did.
#   MUSIL_FLTK_TARBALL   a local copy of the source tarball, used instead of downloading (optional)
set(MUSIL_FLTK_VERSION 1.4.4)
set(MUSIL_FLTK_URL https://github.com/fltk/fltk/releases/download/release-${MUSIL_FLTK_VERSION}/fltk-${MUSIL_FLTK_VERSION}-source.tar.gz)
set(MUSIL_FLTK_SHA256 94b464cce634182c8407adac1be5fc49678986ca93285699b444352af89b4efe)
set(MUSIL_FLTK_PATCH ${CMAKE_CURRENT_LIST_DIR}/patch-fltk.cmake)

macro(musil_fetch_fltk)
  if(NOT TARGET fltk::fltk)
    include(FetchContent)
    foreach(opt FLTK_BUILD_TEST FLTK_BUILD_EXAMPLES FLTK_BUILD_FLUID FLTK_BUILD_FLTK_OPTIONS FLTK_BUILD_GL
                FLTK_BUILD_FORMS FLTK_BUILD_SHARED_LIBS FLTK_USE_SYSTEM_LIBPNG FLTK_USE_SYSTEM_LIBJPEG
                FLTK_USE_SYSTEM_ZLIB FLTK_USE_PANGO FLTK_BACKEND_WAYLAND)
      set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      set(FLTK_BACKEND_X11 ON CACHE BOOL "" FORCE)
    endif()
    if(POLICY CMP0135)
      cmake_policy(SET CMP0135 NEW)
    endif()
    if(MUSIL_FLTK_TARBALL AND EXISTS "${MUSIL_FLTK_TARBALL}")
      set(_fltk_url "${MUSIL_FLTK_TARBALL}")
    else()
      set(_fltk_url "${MUSIL_FLTK_URL}")
    endif()
    # EXCLUDE_FROM_ALL keeps FLTK out of `cmake --install` (it is linked statically); the patch renames
    # FLTK's own `uninstall` target (fltk-uninstall), so `uninstall` stays Musil's
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
      FetchContent_Declare(fltk URL "${_fltk_url}" URL_HASH SHA256=${MUSIL_FLTK_SHA256}
        PATCH_COMMAND ${CMAKE_COMMAND} -P ${MUSIL_FLTK_PATCH} EXCLUDE_FROM_ALL)
      FetchContent_MakeAvailable(fltk)
    else()
      FetchContent_Declare(fltk URL "${_fltk_url}" URL_HASH SHA256=${MUSIL_FLTK_SHA256}
        PATCH_COMMAND ${CMAKE_COMMAND} -P ${MUSIL_FLTK_PATCH})
      FetchContent_GetProperties(fltk)
      if(NOT fltk_POPULATED)
        FetchContent_Populate(fltk)
        add_subdirectory(${fltk_SOURCE_DIR} ${fltk_BINARY_DIR} EXCLUDE_FROM_ALL)
      endif()
    endif()
    message(STATUS "FLTK ${MUSIL_FLTK_VERSION}: built with the program, statically")
  endif()
endmacro()
