# uninstall.cmake — remove what `cmake --install` put in place, without conditions:
#   <prefix>/bin/musil, musil-ide, <prefix>/share/musil/, <prefix>/include/musil/, and in ~/.musil only the
#   libraries an older install copied there (LIB_NAMES): the user's datasets.mu and tokens stay
# and, when a manifest of the last install exists, every file it lists (covers an install
# made with --prefix somewhere else).
set(n 0)
function(gone f)
  if(EXISTS "${f}" OR IS_SYMLINK "${f}")
    message(STATUS "removing ${f}")
    file(REMOVE_RECURSE "${f}")
    math(EXPR n "${n} + 1")
    set(n ${n} PARENT_SCOPE)
  endif()
endfunction()

gone("${PREFIX}/bin/musil")
gone("${PREFIX}/bin/musil-ide")
gone("${PREFIX}/include/musil")
gone("${PREFIX}/share/musil")
string(REPLACE "," ";" lib_names "${LIB_NAMES}")   # names separated by commas
foreach(name ${lib_names})
  gone("${MUSIL_HOME}/${name}")
endforeach()
if(EXISTS "${MANIFEST}")
  file(STRINGS "${MANIFEST}" files)
  foreach(f ${files})
    gone("${f}")
    if(f MATCHES "^(.*)/include/musil/")      # an install made elsewhere: remove that include/musil too
      gone("${CMAKE_MATCH_1}/include/musil")
    endif()
  endforeach()
  file(REMOVE "${MANIFEST}")
endif()
message(STATUS "uninstall: ${n} item(s) removed from ${PREFIX} and ${MUSIL_HOME}")
