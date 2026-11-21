# install_test.cmake — an install puts the libraries beside the program, where it finds them with no ~/.musil;
# uninstall removes them and the libraries an older install left in ~/.musil, never the user's datasets.mu.
#   cmake -DBUILD=<musil's build dir> -DSRC=<musil's source dir> -DWORK=<scratch dir> -P install_test.cmake
file(REMOVE_RECURSE "${WORK}")
set(prefix "${WORK}/prefix")
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${home}")
execute_process(COMMAND ${CMAKE_COMMAND} --install "${BUILD}" --prefix "${prefix}" RESULT_VARIABLE r OUTPUT_QUIET)
if(NOT r EQUAL 0)
  message(FATAL_ERROR "install failed")
endif()
if(NOT EXISTS "${prefix}/share/musil/music.mu" OR NOT EXISTS "${prefix}/share/musil/help.txt")
  message(FATAL_ERROR "the libraries are not in <prefix>/share/musil")
endif()
if(EXISTS "${home}/.musil")
  message(FATAL_ERROR "the install wrote into ~/.musil")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E env HOME=${home} USERPROFILE=${home}
                        "${prefix}/bin/musil" -e "load \"music.mu\"" -e "print (defined? \"play-score\")"
                OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "^1")
  message(FATAL_ERROR "the installed musil does not find its libraries: ${out} ${err}")
endif()
# an older install's libraries in ~/.musil, and the user's datasets: uninstall takes the first, keeps the second
file(WRITE "${home}/.musil/music.mu" "; old\n")
file(WRITE "${home}/.musil/datasets.mu" "folder \"/data\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -DPREFIX=${prefix} -DMUSIL_HOME=${home}/.musil
                        "-DLIB_NAMES=std.mu,system.mu,music.mu,signals.mu,scientific.mu,plot.mu,live.mu,help.txt"
                        -P "${SRC}/cmake/uninstall.cmake" OUTPUT_QUIET)
if(EXISTS "${prefix}/share/musil" OR EXISTS "${prefix}/bin/musil")
  message(FATAL_ERROR "uninstall left the program or its libraries")
endif()
if(EXISTS "${home}/.musil/music.mu")
  message(FATAL_ERROR "uninstall left an older install's library in ~/.musil")
endif()
if(NOT EXISTS "${home}/.musil/datasets.mu")
  message(FATAL_ERROR "uninstall removed the user's datasets.mu")
endif()
file(REMOVE_RECURSE "${WORK}")
message(STATUS "install and uninstall: PASS")
