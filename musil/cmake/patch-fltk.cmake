# patch-fltk.cmake — run in FLTK's source tree when it is fetched: its `uninstall` target becomes
# `fltk-uninstall`, so the build that includes FLTK keeps the name (cmake -P, so it works everywhere)
set(f CMake/install.cmake)
file(READ ${f} text)
string(REPLACE "add_custom_target(uninstall" "add_custom_target(fltk-uninstall" text "${text}")
file(WRITE ${f} "${text}")
