# stage.cmake — puts Moritz together, run by the build (cmake -P, so it works on every system):
#
#   MODE=stage     the program's folder, ready to run: build/Moritz.app (signed) on macOS, build/Moritz/
#                  on Linux and Windows (where the build already put moritz and musil/musil); only what
#                  changed is copied
#   MODE=dist      the download: build/Moritz-<version>-macos-universal.zip (the app, with Musil on its own,
#                  the examples, the documents and the shared data next to it), -linux-x86_64.tar.gz or
#                  -windows-x86_64.zip (the folder), put together afresh
#   MODE=notarize  macOS: signs with SIGN_ID (a Developer ID), notarizes, staples, then MODE=dist
#
# Parameters: SRC (the source tree), BIN (the build tree), PLATFORM (macos, linux, windows), VERSION,
# MORITZ and MUSIL (the built programs), SIGN_ID, NOTARY_PROFILE, MACOS_MIN.

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    string(REPLACE ";" " " cmd "${ARGN}")
    message(FATAL_ERROR "failed (${rc}): ${cmd}")
  endif()
endfunction()

# what every system's folder holds besides the programs. Musil: its libraries (the halves written in
# Musil, their data, its help) and examples, without its C++, and its manual; the shared data; the PDFs
# of docs/ in their folders.
function(copy_resources dest with_readme)
  file(MAKE_DIRECTORY ${dest})
  foreach(d examples library help tutorials)
    file(COPY ${SRC}/${d} DESTINATION ${dest} PATTERN .DS_Store EXCLUDE)
  endforeach()
  file(GLOB_RECURSE pdfs RELATIVE ${SRC} ${SRC}/docs/*.pdf)
  foreach(f ${pdfs})
    get_filename_component(dir ${f} DIRECTORY)
    file(COPY ${SRC}/${f} DESTINATION ${dest}/${dir})
  endforeach()
  file(GLOB musil_lib ${SRC}/musil/src/*.mu ${SRC}/musil/src/*.csv ${SRC}/musil/src/*.wav ${SRC}/musil/src/help.txt)
  file(COPY ${musil_lib} DESTINATION ${dest}/musil/src)
  file(GLOB musil_ex ${SRC}/musil/examples/*.mu)
  file(COPY ${musil_ex} DESTINATION ${dest}/musil/examples)
  file(COPY ${SRC}/musil/docs/musil_manual.pdf DESTINATION ${dest}/musil/docs)
  file(COPY ${SRC}/data DESTINATION ${dest} PATTERN .DS_Store EXCLUDE)
  file(COPY ${SRC}/LICENSE DESTINATION ${dest})
  if(with_readme)
    file(COPY ${SRC}/README.md ${SRC}/OBJECTS.md DESTINATION ${dest})
  endif()
endfunction()

# macOS: the bundle, signed (ad hoc unless SIGN_ID names a Developer ID)
function(make_bundle app)
  file(MAKE_DIRECTORY ${app}/Contents/MacOS ${app}/Contents/Resources)
  file(COPY ${MORITZ} DESTINATION ${app}/Contents/MacOS)
  copy_resources(${app}/Contents/Resources OFF)
  file(COPY ${SRC}/resources/moritz.icns ${SRC}/resources/about.png ${SRC}/resources/maxundmoritz.png
       DESTINATION ${app}/Contents/Resources)
  set(MORITZ_VERSION ${VERSION})
  configure_file(${SRC}/resources/Info.plist.in ${app}/Contents/Info.plist @ONLY)
  file(WRITE ${app}/Contents/PkgInfo "APPL????")
  run(xattr -cr ${app})                   # Finder and quarantine detritus would break the signature
  if(SIGN_ID STREQUAL "-")
    run(codesign --force --deep --sign - ${app})
  else()
    run(codesign --force --deep --options runtime --timestamp --entitlements ${SRC}/resources/moritz.entitlements
        --sign ${SIGN_ID} ${app})
  endif()
  run(codesign --verify --deep --strict ${app})
endfunction()

# Linux and Windows: the folder; the programs are copied into it when it is not the build's own
function(make_folder dir)
  if(NOT EXISTS ${dir}/musil)
    file(MAKE_DIRECTORY ${dir}/musil)
  endif()
  get_filename_component(mdir ${MORITZ} DIRECTORY)
  if(NOT mdir STREQUAL dir)
    file(COPY ${MORITZ} DESTINATION ${dir})
    file(COPY ${MUSIL} DESTINATION ${dir}/musil)
  endif()
  copy_resources(${dir} ON)
  file(COPY ${SRC}/resources/about.png ${SRC}/resources/maxundmoritz.png DESTINATION ${dir}/resources)
endfunction()

set(NAME Moritz-${VERSION})

if(MODE STREQUAL "stage")
  if(PLATFORM STREQUAL "macos")
    make_bundle(${BIN}/Moritz.app)
  else()
    make_folder(${BIN}/Moritz)
  endif()

elseif(MODE STREQUAL "dist")
  set(out ${BIN}/dist)
  file(REMOVE_RECURSE ${out})
  if(PLATFORM STREQUAL "macos")
    # the app, and next to it Musil on its own, the examples, the documents, the shared data
    set(d ${out}/${NAME})
    file(MAKE_DIRECTORY ${d})
    run(ditto ${BIN}/Moritz.app ${d}/Moritz.app)
    copy_resources(${d} ON)
    file(REMOVE_RECURSE ${d}/library ${d}/help ${d}/tutorials)      # they are in the app
    file(COPY ${MUSIL} DESTINATION ${d}/musil)
    run(xattr -cr ${out})
    run(codesign --verify --deep --strict ${d}/Moritz.app)
    set(zip ${BIN}/${NAME}-macos-universal.zip)
    file(REMOVE ${zip})
    # --norsrc: no resource forks or extended attributes, which would travel as ._ files and could
    # invalidate the signature once unzipped on another Mac
    execute_process(COMMAND ditto -c -k --norsrc --keepParent ${NAME} ${zip} WORKING_DIRECTORY ${out} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "ditto failed")
    endif()
  else()
    make_folder(${out}/Moritz)
    if(PLATFORM STREQUAL "linux")
      set(zip ${BIN}/${NAME}-linux-x86_64.tar.gz)
      set(fmt gnutar)
      set(flags czf)
    else()
      set(zip ${BIN}/${NAME}-windows-x86_64.zip)
      set(fmt zip)
      set(flags cf)
    endif()
    file(REMOVE ${zip})
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar ${flags} ${zip} --format=${fmt} Moritz WORKING_DIRECTORY ${out} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "archiving failed")
    endif()
  endif()
  file(SIZE ${zip} size)
  math(EXPR mb "${size} / 1048576")
  message(STATUS "${zip}: ${mb} MB")

elseif(MODE STREQUAL "notarize")
  if(SIGN_ID STREQUAL "-")
    message(FATAL_ERROR "notarize needs a Developer ID: cmake -B build -DMORITZ_SIGN_ID=\"Developer ID Application: Name (TEAMID)\"")
  endif()
  set(zip ${BIN}/notarize.zip)
  file(REMOVE ${zip})
  run(ditto -c -k --norsrc --keepParent ${BIN}/Moritz.app ${zip})
  run(xcrun notarytool submit ${zip} --keychain-profile ${NOTARY_PROFILE} --wait)
  run(xcrun stapler staple ${BIN}/Moritz.app)
  run(spctl --assess --type execute --verbose ${BIN}/Moritz.app)
  execute_process(COMMAND ${CMAKE_COMMAND} -DMODE=dist -DSRC=${SRC} -DBIN=${BIN} -DPLATFORM=${PLATFORM} -DVERSION=${VERSION}
                  -DMORITZ=${MORITZ} -DMUSIL=${MUSIL} -DSIGN_ID=${SIGN_ID} -P ${CMAKE_CURRENT_LIST_FILE})
else()
  message(FATAL_ERROR "MODE is stage, dist or notarize")
endif()
