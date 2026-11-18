# Moritz — a Max-like environment for sound and music computing, and Musil, its language.
# One build for both, on macOS, Linux and Windows:
#
#   cmake -B build                  configure (downloads FLTK, miniaudio and RtMidi into deps/ once)
#   cmake --build build -j8         Moritz, ready to run, and Musil on its own:
#                                     macOS    build/Moritz.app (universal: arm64 + x86_64, macOS 11+)
#                                              and build/musil/musil
#                                     Linux    build/Moritz/moritz and build/Moritz/musil/musil
#                                     Windows  build/Moritz/moritz.exe and build/Moritz/musil/musil.exe
#   ctest --test-dir build          every patch rendered, Musil's tests on Musil and on Musil in Moritz
#   cmake --build build --target dist      the download of this system, in build/
#   cmake --install build           Moritz (macOS: /Applications; Linux: <prefix>/lib/moritz, with
#                                   <prefix>/bin/moritz; Windows: <prefix>/Moritz) and Musil
#                                   (<prefix>/bin/musil, its libraries in ~/.musil, its headers)
#
# Windows: in an MSYS2 MINGW64 shell, cmake -G "MSYS Makefiles" -B build (packages: make curl
# mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake); from macOS or Linux, cross-compiled with MinGW-w64:
# cmake -B build/windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake.
# Linux packages (Debian/Ubuntu): build-essential cmake curl libasound2-dev libx11-dev libxext-dev
# libxft-dev libxinerama-dev libxcursor-dev libxrender-dev libxfixes-dev libfontconfig1-dev
#
# The version is in the file VERSION (-DMORITZ_VERSION=... overrides it, as the release build does).
# Makefile is a shortcut to these commands.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "the oldest macOS")
endif()

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION MORITZ_VERSION_FILE LIMIT_COUNT 1)
set(MORITZ_VERSION "${MORITZ_VERSION_FILE}" CACHE STRING "the version, shown in About and in the downloads' names")
project(Moritz LANGUAGES C CXX)
message(STATUS "Moritz ${MORITZ_VERSION}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# -DMORITZ_SANITIZE=address (ASan + UBSan) or thread (TSan): every target, FLTK included
set(MORITZ_SANITIZE "" CACHE STRING "sanitizer build: address or thread (empty: none)")
if(MORITZ_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g1)   # line tables: enough for the reports
  add_link_options(-fsanitize=address,undefined)
elseif(MORITZ_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g1)
  add_link_options(-fsanitize=thread)
elseif(NOT MORITZ_SANITIZE STREQUAL "")
  message(FATAL_ERROR "MORITZ_SANITIZE is address or thread")
endif()

if(APPLE)
  set(MORITZ_PLATFORM macos)
elseif(WIN32)
  set(MORITZ_PLATFORM windows)
else()
  set(MORITZ_PLATFORM linux)
endif()

# ---------------------------------------------------------------- dependencies
# Downloaded once into deps/ (kept between builds and cached by CI); FLTK is built with the program.
set(MORITZ_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/deps CACHE PATH "the downloaded dependencies")
set(MA_VERSION 0.11.22)
set(RTMIDI_VERSION 6.0.0)
# each file is checked against its SHA-256, downloaded or already there
function(moritz_download url file sha256)
  if(EXISTS ${file})
    file(SHA256 ${file} have)
    if(NOT have STREQUAL sha256)
      message(STATUS "${file} does not match its checksum: downloading it again")
      file(REMOVE ${file})
    endif()
  endif()
  if(NOT EXISTS ${file})
    message(STATUS "downloading ${url}")
    file(DOWNLOAD ${url} ${file}.part STATUS st TLS_VERIFY ON EXPECTED_HASH SHA256=${sha256})
    list(GET st 0 rc)
    if(NOT rc EQUAL 0)
      file(REMOVE ${file}.part)
      message(FATAL_ERROR "cannot download ${url}: ${st}")
    endif()
    file(RENAME ${file}.part ${file})
  endif()
endfunction()
set(MA_DIR ${MORITZ_DEPS}/miniaudio-${MA_VERSION})
set(RTMIDI_DIR ${MORITZ_DEPS}/rtmidi-${RTMIDI_VERSION})
moritz_download(https://raw.githubusercontent.com/mackron/miniaudio/${MA_VERSION}/miniaudio.h ${MA_DIR}/miniaudio.h
                9019743287e443c55e5737a7297f38e5e358561701d6db2d905afb114390c410)
moritz_download(https://raw.githubusercontent.com/thestk/rtmidi/${RTMIDI_VERSION}/RtMidi.h ${RTMIDI_DIR}/RtMidi.h
                74f98ca172243b0b9c0c59f53c4e20285d879ffa86b3d61692109b013f4606f9)
moritz_download(https://raw.githubusercontent.com/thestk/rtmidi/${RTMIDI_VERSION}/RtMidi.cpp ${RTMIDI_DIR}/RtMidi.cpp
                184d775e88e5d73b079897363fb42af155122e879f0bc9254a1c7edae4eadb3c)

# FLTK, the same for Moritz and Musil (musil/cmake/fltk.cmake), from the tarball in deps/
include(${CMAKE_CURRENT_SOURCE_DIR}/musil/cmake/fltk.cmake)
set(MUSIL_FLTK_TARBALL ${MORITZ_DEPS}/fltk-${MUSIL_FLTK_VERSION}-source.tar.gz)
moritz_download(${MUSIL_FLTK_URL} ${MUSIL_FLTK_TARBALL} ${MUSIL_FLTK_SHA256})
musil_fetch_fltk()

# ---------------------------------------------------------------- where things go
# On Linux and Windows the program is built in its folder, build/Moritz, with everything it reads
# next to it; on macOS it is built in build/bin and put into build/Moritz.app.
if(APPLE)
  set(MORITZ_STAGE ${CMAKE_BINARY_DIR}/Moritz.app)
  set(MORITZ_EXE_DIR ${CMAKE_BINARY_DIR}/bin)
  set(MORITZ_RUN ${MORITZ_STAGE}/Contents/MacOS/moritz)      # the program as it runs, in its bundle
  set(MUSIL_OUTPUT_DIR ${CMAKE_BINARY_DIR}/musil)
else()
  set(MORITZ_STAGE ${CMAKE_BINARY_DIR}/Moritz)
  set(MORITZ_EXE_DIR ${MORITZ_STAGE})
  set(MORITZ_RUN ${MORITZ_STAGE}/moritz${CMAKE_EXECUTABLE_SUFFIX})
  set(MUSIL_OUTPUT_DIR ${MORITZ_STAGE}/musil)
endif()

# ---------------------------------------------------------------- Musil
# Musil builds from musil/ alone (tools/check-musil.sh): here with Moritz's FLTK, without readline
# (the program ships to machines that may not have it), its binary next to its libraries.
set(MUSIL_READLINE OFF CACHE BOOL "" FORCE)
enable_testing()
add_subdirectory(musil)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(musil PRIVATE -static-libstdc++ -static-libgcc)   # runs where another libstdc++ is installed
endif()

# ---------------------------------------------------------------- Moritz
# miniaudio (audio) and RtMidi (MIDI), compiled once, quietly
file(WRITE ${CMAKE_BINARY_DIR}/miniaudio_impl.c "#define MINIAUDIO_IMPLEMENTATION\n#include \"miniaudio.h\"\n")
add_library(moritz_io STATIC ${CMAKE_BINARY_DIR}/miniaudio_impl.c ${RTMIDI_DIR}/RtMidi.cpp)
target_include_directories(moritz_io PUBLIC ${MA_DIR} ${RTMIDI_DIR})
target_compile_options(moritz_io PRIVATE -w)
if(APPLE)
  target_compile_definitions(moritz_io PUBLIC __MACOSX_CORE__)
  target_compile_definitions(moritz_io PRIVATE MA_NO_RUNTIME_LINKING)
elseif(WIN32)
  target_compile_definitions(moritz_io PUBLIC __WINDOWS_MM__)
else()
  target_compile_definitions(moritz_io PUBLIC __LINUX_ALSA__)
endif()

file(GLOB MORITZ_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
if(WIN32)
  enable_language(RC)
  list(APPEND MORITZ_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources/moritz.rc)   # the icon, the UTF-8 manifest
endif()
add_executable(moritz ${MORITZ_SOURCES})
target_include_directories(moritz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/resources)
target_include_directories(moritz SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/musil/src)
target_compile_definitions(moritz PRIVATE MORITZ_VERSION="${MORITZ_VERSION}")
target_compile_options(moritz PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
target_link_libraries(moritz PRIVATE moritz_io fltk::images fltk::fltk)
set_target_properties(moritz PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${MORITZ_EXE_DIR})
if(APPLE)
  target_link_libraries(moritz PRIVATE "-framework Cocoa" "-framework CoreAudio" "-framework AudioToolbox"
                        "-framework CoreMIDI" "-framework CoreFoundation" "-weak_framework UniformTypeIdentifiers")
elseif(WIN32)
  # one .exe without DLLs, a GUI program, an 8 MB stack as on macOS and Linux
  set_target_properties(moritz PROPERTIES WIN32_EXECUTABLE ON)
  target_compile_definitions(moritz PRIVATE NOMINMAX WIN32 UNICODE _UNICODE)
  target_link_options(moritz PRIVATE -static -s -Wl,--stack,8388608)
  target_link_libraries(moritz PRIVATE winmm ole32 uuid comctl32 comdlg32 gdi32 gdiplus winspool ws2_32
                        shell32 advapi32 imm32 oleaut32)
else()
  target_link_options(moritz PRIVATE -static-libstdc++ -static-libgcc)
  target_link_libraries(moritz PRIVATE asound pthread dl m)
endif()

# ---------------------------------------------------------------- the program's folder
# The examples, library, help, tutorials, documents, Musil's libraries and examples and the shared
# data, next to the program (in the bundle on macOS, then signed): cmake/stage.cmake.
set(MORITZ_SIGN_ID "-" CACHE STRING "macOS: the identity to sign with (- signs ad hoc; a Developer ID for a release)")
set(MORITZ_NOTARY_PROFILE "moritz" CACHE STRING "macOS: the notarytool keychain profile")
file(GLOB_RECURSE MORITZ_SHARED CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/examples/* ${CMAKE_CURRENT_SOURCE_DIR}/library/* ${CMAKE_CURRENT_SOURCE_DIR}/help/*
     ${CMAKE_CURRENT_SOURCE_DIR}/tutorials/* ${CMAKE_CURRENT_SOURCE_DIR}/data/* ${CMAKE_CURRENT_SOURCE_DIR}/docs/*.pdf
     ${CMAKE_CURRENT_SOURCE_DIR}/musil/src/*.mu ${CMAKE_CURRENT_SOURCE_DIR}/musil/examples/*.mu
     ${CMAKE_CURRENT_SOURCE_DIR}/resources/*)
set(STAGE_ARGS -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DBIN=${CMAKE_BINARY_DIR} -DPLATFORM=${MORITZ_PLATFORM}
    -DVERSION=${MORITZ_VERSION} -DMORITZ=$<TARGET_FILE:moritz> -DMUSIL=$<TARGET_FILE:musil>
    -DSIGN_ID=${MORITZ_SIGN_ID} -DNOTARY_PROFILE=${MORITZ_NOTARY_PROFILE} -DMACOS_MIN=${CMAKE_OSX_DEPLOYMENT_TARGET})
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/stage.stamp
  COMMAND ${CMAKE_COMMAND} -DMODE=stage ${STAGE_ARGS} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage.cmake
  COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/stage.stamp
  DEPENDS moritz musil ${MORITZ_SHARED} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage.cmake
          ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in ${CMAKE_CURRENT_SOURCE_DIR}/musil/src/help.txt
          ${CMAKE_CURRENT_SOURCE_DIR}/musil/docs/musil_manual.pdf
  COMMENT "Putting together ${MORITZ_STAGE}")
add_custom_target(stage ALL DEPENDS ${CMAKE_BINARY_DIR}/stage.stamp)

# the download of this system, in build/: Moritz-<version>-macos-universal.zip,
# Moritz-<version>-linux-x86_64.tar.gz or Moritz-<version>-windows-x86_64.zip
add_custom_target(dist COMMAND ${CMAKE_COMMAND} -DMODE=dist ${STAGE_ARGS} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage.cmake
                  DEPENDS stage COMMENT "Packaging Moritz ${MORITZ_VERSION}")
if(APPLE)
  # sign with a Developer ID, notarize and staple, then package:
  #   cmake -B build -DMORITZ_SIGN_ID="Developer ID Application: Name (TEAMID)" && cmake --build build --target notarize
  # after storing the credentials once: xcrun notarytool store-credentials moritz --apple-id ... --team-id ...
  add_custom_target(notarize COMMAND ${CMAKE_COMMAND} -DMODE=notarize ${STAGE_ARGS} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage.cmake
                    DEPENDS stage COMMENT "Notarizing Moritz.app")
endif()

# ---------------------------------------------------------------- tests
# Musil's own tests come with musil/; these are Moritz's (sh: on Windows, MSYS2's)
find_program(MORITZ_SH sh)
if(MORITZ_SH)
  add_test(NAME moritz-patches COMMAND ${MORITZ_SH} tools/test-patches.sh ${MORITZ_RUN} .
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME moritz-musil COMMAND ${MORITZ_SH} tools/test-musil.sh moritz ${MORITZ_RUN}
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  # it runs Musil's tests in Moritz, which write the same temporary files as Musil's own: alone
  set_tests_properties(moritz-musil PROPERTIES RUN_SERIAL TRUE)
  add_test(NAME musil-stands-alone COMMAND ${MORITZ_SH} tools/check-musil.sh WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  # the interface driven as a user would (windows, undo, the code view, saving): in a scratch HOME
  add_test(NAME moritz-gui COMMAND ${MORITZ_SH} tools/test-gui.sh ${MORITZ_RUN}
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# The engine's own tests (C++): the program's sources without main.cpp, and tests/engine/*.cpp.
# They run patches from text the way the audio thread does: the allocation guard, edits while
# audio runs (clean under -DMORITZ_SANITIZE=thread), timing, and the fixes that come with tests.
set(MORITZ_ENGINE_TEST_SOURCES ${MORITZ_SOURCES})
list(FILTER MORITZ_ENGINE_TEST_SOURCES EXCLUDE REGEX "/src/main\\.cpp$|\\.rc$")
file(GLOB MORITZ_ENGINE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/engine/*.cpp)
add_executable(moritz-engine-tests ${MORITZ_ENGINE_TEST_SOURCES} ${MORITZ_ENGINE_TESTS})
target_include_directories(moritz-engine-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/resources)
target_include_directories(moritz-engine-tests SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/musil/src)
target_compile_definitions(moritz-engine-tests PRIVATE MORITZ_VERSION="${MORITZ_VERSION}")
target_compile_options(moritz-engine-tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
target_link_libraries(moritz-engine-tests PRIVATE moritz_io fltk::images fltk::fltk)
if(APPLE)
  target_link_libraries(moritz-engine-tests PRIVATE "-framework Cocoa" "-framework CoreAudio" "-framework AudioToolbox"
                        "-framework CoreMIDI" "-framework CoreFoundation" "-weak_framework UniformTypeIdentifiers")
elseif(WIN32)
  target_compile_definitions(moritz-engine-tests PRIVATE NOMINMAX WIN32 UNICODE _UNICODE)
  target_link_options(moritz-engine-tests PRIVATE -static -Wl,--stack,8388608)
  target_link_libraries(moritz-engine-tests PRIVATE winmm ole32 uuid comctl32 comdlg32 gdi32 gdiplus winspool ws2_32
                        shell32 advapi32 imm32 oleaut32)
else()
  target_link_libraries(moritz-engine-tests PRIVATE asound pthread dl m)
endif()
# in its own folder, with the resources the program finds next to itself (the head responses...)
set(MORITZ_ENGINE_TEST_DIR ${CMAKE_BINARY_DIR}/engine-tests)
set_target_properties(moritz-engine-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${MORITZ_ENGINE_TEST_DIR})
file(MAKE_DIRECTORY ${MORITZ_ENGINE_TEST_DIR})
foreach(d musil data library examples help)
  if(NOT EXISTS ${MORITZ_ENGINE_TEST_DIR}/${d})
    file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/${d} ${MORITZ_ENGINE_TEST_DIR}/${d} SYMBOLIC RESULT linked)
  endif()
endforeach()
add_test(NAME moritz-engine COMMAND moritz-engine-tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------- install
if(APPLE)
  set(MORITZ_INSTALL_DIR "/Applications" CACHE PATH "where Moritz.app goes")
  install(DIRECTORY ${MORITZ_STAGE} DESTINATION ${MORITZ_INSTALL_DIR} USE_SOURCE_PERMISSIONS)
elseif(WIN32)
  set(MORITZ_INSTALL_DIR "Moritz" CACHE PATH "where Moritz's folder goes (relative: under the prefix)")
  install(DIRECTORY ${MORITZ_STAGE}/ DESTINATION ${MORITZ_INSTALL_DIR} USE_SOURCE_PERMISSIONS)
else()
  set(MORITZ_INSTALL_DIR "lib/moritz" CACHE PATH "where Moritz's folder goes (relative: under the prefix)")
  install(DIRECTORY ${MORITZ_STAGE}/ DESTINATION ${MORITZ_INSTALL_DIR} USE_SOURCE_PERMISSIONS)
  install(CODE "file(MAKE_DIRECTORY \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin)
    file(CREATE_LINK ../${MORITZ_INSTALL_DIR}/moritz \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/moritz SYMBOLIC)")
endif()
