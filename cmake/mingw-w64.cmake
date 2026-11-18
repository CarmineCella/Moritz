# mingw-w64.cmake — cross-compiling Moritz and Musil for Windows from macOS or Linux with MinGW-w64
# (brew install mingw-w64, or apt install mingw-w64):
#   cmake -B build/windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake && cmake --build build/windows -j8
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MINGW_PREFIX x86_64-w64-mingw32 CACHE STRING "the MinGW-w64 tools' prefix")
set(CMAKE_C_COMPILER ${MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${MINGW_PREFIX}-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
