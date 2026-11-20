# Moritz — shortcuts to the CMake build (CMakeLists.txt), which builds Moritz and Musil on every system.
#
#   make             configure (once) and build: build/Moritz.app and build/musil/musil on macOS,
#                    build/Moritz/ (with musil/) on Linux and Windows (MSYS2)
#   make run         build and open Moritz
#   make test        every test: the patches, Musil's tests on Musil and on Musil in Moritz (ctest)
#   make dist        the download of this system, in build/
#   make windows     macOS or Linux: cross-compiled for Windows with MinGW-w64, in build/windows/Moritz/;
#                    make windows-dist zips it
#   make install     Moritz and Musil into the system (cmake --install; may need sudo)
#   make notarize SIGN_ID="Developer ID Application: Name (TEAMID)"   macOS: sign, notarize, package
#   make clean       the build; make distclean also the downloaded dependencies (deps/)
#
# VERSION=x overrides the file VERSION; BUILD=dir builds elsewhere; JOBS=n (8).

BUILD ?= build
JOBS  ?= 8
UNAME := $(shell uname -s)
CMAKE_ARGS :=
ifdef VERSION
CMAKE_ARGS += -DMORITZ_VERSION=$(VERSION)
endif
ifdef SIGN_ID
CMAKE_ARGS += -DMORITZ_SIGN_ID="$(SIGN_ID)"
endif
ifneq (,$(findstring _NT,$(UNAME)))          # MSYS2
CMAKE_ARGS += -G "MSYS Makefiles"
endif

# configured once (cmake --build configures again by itself when a CMake file changes); VERSION= or SIGN_ID=
# configure again; make configure does it by hand
ifneq (,$(VERSION)$(SIGN_ID))
all: configure
else
all: $(BUILD)/CMakeCache.txt
endif
	cmake --build $(BUILD) -j$(JOBS)

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD) $(CMAKE_ARGS)

configure:
	cmake -S . -B $(BUILD) $(CMAKE_ARGS)

run: all
ifeq ($(UNAME),Darwin)
	open $(BUILD)/Moritz.app
else
	$(BUILD)/Moritz/moritz
endif

test: all
	ctest --test-dir $(BUILD) -j4 --output-on-failure

dist: all
	cmake --build $(BUILD) --target dist

notarize: configure
	cmake --build $(BUILD) -j$(JOBS) --target notarize

install: all
	cmake --install $(BUILD)

windows:
	cmake -S . -B $(BUILD)/windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake $(CMAKE_ARGS)
	cmake --build $(BUILD)/windows -j$(JOBS)

windows-dist: windows
	cmake --build $(BUILD)/windows --target dist

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf deps

.PHONY: all configure run test dist notarize install windows windows-dist clean distclean
