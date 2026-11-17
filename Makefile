# Moritz — a Max-like environment for sound and music computing
#
#   make          download + build deps (first run only), then build:
#                 on macOS Moritz.app, on Linux build/linux/Moritz/ (= make linux),
#                 on Windows (MSYS2) build/windows/Moritz/ (= make windows)
#   make run      build and open the app
#   make linux    the Linux build: build/linux/Moritz/moritz with the examples,
#                 library and help next to it (needs the packages listed below)
#   make windows  the Windows build: build/windows/Moritz/moritz.exe and its
#                 folders. On Windows, in an MSYS2 MINGW64 shell, plain `make`
#                 does it (packages listed below); on macOS or Linux it
#                 cross-compiles with MinGW-w64 (brew install mingw-w64, or
#                 apt install mingw-w64)
#   make windows-zip  the same, zipped for a release
#   make linux-dist   the Linux build as a .tar.gz for a release
#   make dist     macOS: build the release zip (app + examples + license) in build/
#   make notarize macOS: sign with a Developer ID, notarize and staple (see below)
#   make musil    Musil on its own (musil/): the musil command line, with its live audio,
#                 plots and controls, in build/musil/ (Linux: build/linux/musil/,
#                 Windows: build/windows/musil/)
#   make test-musil  Musil's tests on Musil itself, then on Musil in Moritz
#   make clean    remove build products
#   make distclean  also remove downloaded dependencies
#
# On macOS the app is a universal (arm64 + x86_64) binary statically linked
# with FLTK. Dependencies are downloaded into deps/ on the first build: FLTK
# (built with cmake), miniaudio (audio) and RtMidi (MIDI).
#
# Windows (MSYS2, https://www.msys2.org, in the "MSYS2 MINGW64" shell):
#   pacman -S make curl zip mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake
#
# Linux packages (Debian/Ubuntu names): build-essential cmake curl
#   libasound2-dev libx11-dev libxext-dev libxft-dev libxinerama-dev
#   libxcursor-dev libxrender-dev libxfixes-dev libfontconfig1-dev

APP        := moritz
BUNDLE     := Moritz.app
VERSION    := 0.10
UNAME      := $(shell uname -s)
# MSYS2 on Windows reports MINGW64_NT-..., UCRT64_NT-..., MSYS_NT-...
ifneq (,$(findstring _NT,$(UNAME)))
WINDOWS_HOST := 1
endif

FLTK_VER   := 1.4.4
FLTK_URL   := https://github.com/fltk/fltk/releases/download/release-$(FLTK_VER)/fltk-$(FLTK_VER)-source.tar.gz
MA_VER     := 0.11.22
MA_URL     := https://raw.githubusercontent.com/mackron/miniaudio/$(MA_VER)/miniaudio.h
RTMIDI_VER := 6.0.0
RTMIDI_URL := https://raw.githubusercontent.com/thestk/rtmidi/$(RTMIDI_VER)

DEPS       := deps
BUILD      := build
FLTK_TGZ   := $(DEPS)/fltk-$(FLTK_VER)-source.tar.gz
FLTK_SRC   := $(DEPS)/fltk-$(FLTK_VER)
FLTK_BLD   := $(DEPS)/fltk-build
FLTK_INST  := $(DEPS)/fltk
FLTK_LIB   := $(FLTK_INST)/lib/libfltk.a
FLTK_LIBS  := $(FLTK_INST)/lib/libfltk_images.a $(FLTK_INST)/lib/libfltk_png.a \
              $(FLTK_INST)/lib/libfltk_z.a $(FLTK_LIB)
MA_DIR     := $(DEPS)/miniaudio-$(MA_VER)
MA_HDR     := $(MA_DIR)/miniaudio.h
RTMIDI_DIR := $(DEPS)/rtmidi-$(RTMIDI_VER)
RTMIDI_SRC := $(RTMIDI_DIR)/RtMidi.cpp
RTMIDI_HDR := $(RTMIDI_DIR)/RtMidi.h

# Code signing. By default the bundle is signed ad-hoc ("-"): it runs on the
# machine that built it, but macOS blocks it when downloaded. For a release
# that opens anywhere, sign with your Developer ID and notarize:
#   make notarize SIGN_ID="Developer ID Application: Your Name (TEAMID)" \
#                 NOTARY_PROFILE=moritz
# after storing notary credentials once with
#   xcrun notarytool store-credentials moritz --apple-id you@example.com \
#         --team-id TEAMID --password <app-specific password>
SIGN_ID        ?= -
NOTARY_PROFILE ?= moritz
ENTITLEMENTS   := resources/moritz.entitlements
ifeq ($(SIGN_ID),-)
CODESIGN_FLAGS := --force --deep --sign -
else
CODESIGN_FLAGS := --force --deep --options runtime --timestamp --entitlements $(ENTITLEMENTS) \
                  --sign "$(SIGN_ID)"
endif
DIST_NAME  := Moritz-$(VERSION)
DIST_ZIP   := $(BUILD)/$(DIST_NAME)-macos-universal.zip

ARCHS      := arm64 x86_64
MACOS_MIN  := 11.0
ARCHFLAGS  := $(addprefix -arch ,$(ARCHS)) -mmacosx-version-min=$(MACOS_MIN)

CXX        := clang++
CC         := clang
CXXFLAGS   := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter $(ARCHFLAGS) \
              -I$(FLTK_INST)/include -I$(MA_DIR) -I$(RTMIDI_DIR) -Isrc -isystem musil/src -DMORITZ_VERSION=\"$(VERSION)\"
MAFLAGS    := -O2 $(ARCHFLAGS) -DMA_NO_RUNTIME_LINKING
FRAMEWORKS := -framework Cocoa -framework CoreAudio -framework AudioToolbox -framework CoreMIDI \
              -framework CoreFoundation -weak_framework UniformTypeIdentifiers
LDFLAGS    := $(ARCHFLAGS)

ifeq ($(UNAME),Linux)
all: linux
else ifdef WINDOWS_HOST
all: windows
else
all: $(BUNDLE)
endif

# ---------------------------------------------------------------- dependencies
deps: $(FLTK_LIB) $(MA_HDR) $(RTMIDI_HDR)

$(FLTK_TGZ):
	@mkdir -p $(DEPS)
	@echo "==> downloading FLTK $(FLTK_VER)"
	curl -fL --retry 3 -o $@.part $(FLTK_URL)
	mv $@.part $@

$(FLTK_LIB): $(FLTK_TGZ)
	@echo "==> building FLTK $(FLTK_VER) (static, $(ARCHS))"
	rm -rf $(FLTK_SRC) $(FLTK_BLD) $(FLTK_INST)
	tar -xzf $(FLTK_TGZ) -C $(DEPS)
	cmake -S $(FLTK_SRC) -B $(FLTK_BLD) \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_INSTALL_PREFIX=$(abspath $(FLTK_INST)) \
	  -DCMAKE_OSX_ARCHITECTURES="$(subst $() ,;,$(ARCHS))" \
	  -DCMAKE_OSX_DEPLOYMENT_TARGET=$(MACOS_MIN) \
	  -DFLTK_BUILD_SHARED_LIBS=OFF \
	  -DFLTK_BUILD_GL=OFF \
	  -DFLTK_BUILD_TEST=OFF \
	  -DFLTK_BUILD_EXAMPLES=OFF \
	  -DFLTK_BUILD_FLUID=OFF \
	  -DFLTK_BUILD_FLTK_OPTIONS=OFF \
	  -DFLTK_USE_SYSTEM_LIBJPEG=OFF \
	  -DFLTK_USE_SYSTEM_LIBPNG=OFF \
	  -DFLTK_USE_SYSTEM_ZLIB=OFF \
	  -DFLTK_BACKEND_X11=OFF
	cmake --build $(FLTK_BLD) --parallel
	cmake --install $(FLTK_BLD)
	@touch $@

$(MA_HDR):
	@mkdir -p $(MA_DIR)
	@echo "==> downloading miniaudio $(MA_VER)"
	curl -fL --retry 3 -o $@.part $(MA_URL)
	mv $@.part $@

$(RTMIDI_HDR):
	@mkdir -p $(RTMIDI_DIR)
	@echo "==> downloading RtMidi $(RTMIDI_VER)"
	curl -fL --retry 3 -o $(RTMIDI_SRC).part $(RTMIDI_URL)/RtMidi.cpp
	curl -fL --retry 3 -o $@.part $(RTMIDI_URL)/RtMidi.h
	mv $(RTMIDI_SRC).part $(RTMIDI_SRC)
	mv $@.part $@

# ---------------------------------------------------------------- program (macOS)
# miniaudio's implementation is compiled straight from its header, once.
$(BUILD)/miniaudio.o: $(MA_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(MAFLAGS) -x c -DMINIAUDIO_IMPLEMENTATION -c $(MA_HDR) -o $@

$(BUILD)/RtMidi.o: $(RTMIDI_HDR)
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O2 $(ARCHFLAGS) -D__MACOSX_CORE__ -w -c $(RTMIDI_SRC) -o $@

# Musil's libraries (the halves written in Musil, their data and its help) and its examples, without its
# C++ sources; the sounds and data shared by Moritz and Musil; Musil's manual (in Help > Documents too).
# The destination is a fresh folder (data/ is copied into it).
copy_musil = mkdir -p "$(1)/musil/src" "$(1)/musil/examples" "$(1)/musil/docs" && \
	cp musil/src/*.mu musil/src/*.csv musil/src/*.wav musil/src/help.txt "$(1)/musil/src/" && \
	cp musil/examples/*.mu "$(1)/musil/examples/" && cp musil/docs/musil_manual.pdf "$(1)/musil/docs/" && \
	cp -R data "$(1)/" && find "$(1)/data" -name .DS_Store -delete

# the PDFs of docs/ (their LaTeX sources stay in the repository), keeping their folders
copy_docs = for f in $$(find docs -name '*.pdf'); do mkdir -p "$(1)/$$(dirname $$f)"; cp "$$f" "$(1)/$$f"; done

SOURCES    := $(wildcard src/*.cpp)
OBJECTS    := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SOURCES))
HEADERS    := $(wildcard src/*.h musil/src/*.h musil/src/*/*.h)
MUSIL_DEPS := musil/cli/main.cpp $(wildcard musil/src/*.h musil/src/*/*.h)
SHARED     := $(wildcard examples/*/*.moritz data/* library/* musil/src/*.mu musil/src/help.txt musil/examples/*.mu \
              help/* tutorials/* docs/*/*.pdf) musil/docs/musil_manual.pdf

$(BUILD)/%.o: src/%.cpp $(HEADERS) $(FLTK_LIB) $(MA_HDR) $(RTMIDI_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -D__MACOSX_CORE__ -c $< -o $@

$(APP): $(OBJECTS) $(BUILD)/miniaudio.o $(BUILD)/RtMidi.o
	$(CXX) $(LDFLAGS) $^ $(FLTK_LIBS) $(FRAMEWORKS) -o $@

# Musil on its own, built from musil/ alone (its own miniaudio, inside live.h), with Moritz's FLTK
MUSIL_BIN := $(BUILD)/musil/musil
$(MUSIL_BIN): $(MUSIL_DEPS) $(FLTK_LIB)
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter $(ARCHFLAGS) -DMUSIL_HAS_FLTK -I$(FLTK_INST)/include \
	  -Imusil/src musil/cli/main.cpp $(FLTK_LIBS) -framework Cocoa -framework CoreAudio -framework AudioToolbox \
	  -framework CoreFoundation -weak_framework UniformTypeIdentifiers -o $@

# ---------------------------------------------------------------- bundle (macOS)
define INFO_PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Moritz</string>
  <key>CFBundleDisplayName</key><string>Moritz</string>
  <key>CFBundleExecutable</key><string>$(APP)</string>
  <key>CFBundleIdentifier</key><string>org.moritz.moritz</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>$(VERSION)</string>
  <key>CFBundleShortVersionString</key><string>$(VERSION)</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>LSMinimumSystemVersion</key><string>$(MACOS_MIN)</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>CFBundleIconFile</key><string>moritz</string>
  <key>NSHumanReadableCopyright</key><string>Created by Carmine-Emanuele Cella (2026). BSD 2-Clause License.</string>
  <key>NSMicrophoneUsageDescription</key><string>Moritz uses audio input for the adc~ object.</string>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>Moritz patch</string>
      <key>CFBundleTypeRole</key><string>Editor</string>
      <key>CFBundleTypeExtensions</key><array><string>moritz</string><string>mlsp</string></array>
    </dict>
  </array>
</dict>
</plist>
endef
export INFO_PLIST

$(BUNDLE): $(APP) $(SHARED) $(wildcard resources/*) LICENSE
	@echo "==> creating $(BUNDLE)"
	rm -rf $(BUNDLE)
	mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources
	cp $(APP) $(BUNDLE)/Contents/MacOS/$(APP)
	cp -R examples library help tutorials $(BUNDLE)/Contents/Resources/ 2>/dev/null || true
	$(call copy_docs,$(BUNDLE)/Contents/Resources)
	$(call copy_musil,$(BUNDLE)/Contents/Resources)
	cp resources/moritz.icns resources/about.png resources/maxundmoritz.png LICENSE $(BUNDLE)/Contents/Resources/
	echo "$$INFO_PLIST" > $(BUNDLE)/Contents/Info.plist
	printf 'APPL????' > $(BUNDLE)/Contents/PkgInfo
	xattr -cr $(BUNDLE)          # Finder/quarantine detritus would break the signature
	codesign $(CODESIGN_FLAGS) $(BUNDLE)
	codesign --verify --deep --strict $(BUNDLE)
	@lipo -info $(BUNDLE)/Contents/MacOS/$(APP)

# ---------------------------------------------------------------- Linux
# FLTK is built for X11 in its own folder, so a macOS and a Linux build can
# share one deps/. The program goes in build/linux/Moritz/ with the examples,
# library and help next to it (the program finds them there).
LX_BUILD    := $(BUILD)/linux
LX_DIR      := $(LX_BUILD)/Moritz
LX_FLTK_BLD := $(DEPS)/fltk-linux-build
LX_FLTK     := $(DEPS)/fltk-linux
LX_FLTK_LIB := $(LX_FLTK)/lib/libfltk.a
LX_CXX      ?= g++
LX_CC       ?= gcc
LX_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I$(LX_FLTK)/include \
               -I$(MA_DIR) -I$(RTMIDI_DIR) -Isrc -isystem musil/src -DMORITZ_VERSION=\"$(VERSION)\" -D__LINUX_ALSA__
LX_OBJECTS  := $(patsubst src/%.cpp,$(LX_BUILD)/%.o,$(SOURCES))

$(LX_FLTK_LIB): $(FLTK_TGZ)
	@echo "==> building FLTK $(FLTK_VER) for Linux (static, X11)"
	rm -rf $(LX_FLTK_BLD) $(LX_FLTK)
	[ -d $(FLTK_SRC) ] || tar -xzf $(FLTK_TGZ) -C $(DEPS)
	cmake -S $(FLTK_SRC) -B $(LX_FLTK_BLD) \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_INSTALL_PREFIX=$(abspath $(LX_FLTK)) \
	  -DFLTK_BUILD_SHARED_LIBS=OFF \
	  -DFLTK_BUILD_GL=OFF \
	  -DFLTK_BUILD_TEST=OFF \
	  -DFLTK_BUILD_EXAMPLES=OFF \
	  -DFLTK_BUILD_FLUID=OFF \
	  -DFLTK_BUILD_FLTK_OPTIONS=OFF \
	  -DFLTK_USE_SYSTEM_LIBJPEG=OFF \
	  -DFLTK_USE_SYSTEM_LIBPNG=OFF \
	  -DFLTK_USE_SYSTEM_ZLIB=OFF \
	  -DFLTK_BACKEND_WAYLAND=OFF \
	  -DFLTK_BACKEND_X11=ON \
	  -DFLTK_USE_PANGO=OFF
	cmake --build $(LX_FLTK_BLD) --parallel
	cmake --install $(LX_FLTK_BLD)
	@touch $@

$(LX_BUILD)/miniaudio.o: $(MA_HDR)
	@mkdir -p $(LX_BUILD)
	$(LX_CC) -O2 -x c -DMINIAUDIO_IMPLEMENTATION -c $(MA_HDR) -o $@

$(LX_BUILD)/RtMidi.o: $(RTMIDI_HDR)
	@mkdir -p $(LX_BUILD)
	$(LX_CXX) -std=c++17 -O2 -D__LINUX_ALSA__ -w -c $(RTMIDI_SRC) -o $@

$(LX_BUILD)/%.o: src/%.cpp $(HEADERS) $(LX_FLTK_LIB) $(MA_HDR) $(RTMIDI_HDR)
	@mkdir -p $(LX_BUILD)
	$(LX_CXX) $(LX_CXXFLAGS) -c $< -o $@

$(LX_DIR)/$(APP): $(LX_OBJECTS) $(LX_BUILD)/miniaudio.o $(LX_BUILD)/RtMidi.o
	@mkdir -p $(LX_DIR)
	$(LX_CXX) $^ $$($(LX_FLTK)/bin/fltk-config --use-images --ldstaticflags) \
	  -static-libstdc++ -static-libgcc -lasound -lpthread -ldl -lm -o $@

LX_MUSIL := $(LX_BUILD)/musil/musil
$(LX_MUSIL): $(MUSIL_DEPS) $(LX_FLTK_LIB)
	@mkdir -p $(dir $@)
	$(LX_CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -DMUSIL_HAS_FLTK -I$(LX_FLTK)/include -Imusil/src \
	  musil/cli/main.cpp $$($(LX_FLTK)/bin/fltk-config --use-images --ldstaticflags) \
	  -static-libstdc++ -static-libgcc -lpthread -ldl -lm -o $@

linux: $(LX_DIR)/$(APP) $(LX_MUSIL) $(SHARED)
	rm -rf $(LX_DIR)/examples $(LX_DIR)/library $(LX_DIR)/musil $(LX_DIR)/help $(LX_DIR)/tutorials $(LX_DIR)/resources
	rm -rf $(LX_DIR)/docs $(LX_DIR)/data
	cp -R examples library help tutorials LICENSE README.md OBJECTS.md $(LX_DIR)/
	$(call copy_docs,$(LX_DIR))
	$(call copy_musil,$(LX_DIR))
	cp $(LX_MUSIL) $(LX_DIR)/musil/
	mkdir -p $(LX_DIR)/resources && cp resources/about.png resources/maxundmoritz.png $(LX_DIR)/resources/
	@echo "==> built $(LX_DIR)/$(APP)"

LX_TGZ := $(BUILD)/$(DIST_NAME)-linux-x86_64.tar.gz
linux-dist: linux
	tar -czf $(LX_TGZ) -C $(LX_BUILD) Moritz
	@ls -l $(LX_TGZ)

# ---------------------------------------------------------------- Windows
# Cross-compiled with MinGW-w64 (WIN_PREFIX is the tools' prefix; set it
# empty to build natively in MSYS2). Everything is linked statically, so
# moritz.exe needs no DLLs. The manifest in resources/moritz.rc sets the
# UTF-8 code page, so file names with accents work through fopen and argv.
ifdef WINDOWS_HOST
WIN_PREFIX   ?=
WIN_CMAKE    := -G "MSYS Makefiles"
else
WIN_PREFIX   ?= x86_64-w64-mingw32-
WIN_CMAKE    := -DCMAKE_SYSTEM_NAME=Windows
endif
WIN_BUILD    := $(BUILD)/windows
WIN_DIR      := $(WIN_BUILD)/Moritz
WIN_FLTK_BLD := $(DEPS)/fltk-windows-build
WIN_FLTK     := $(DEPS)/fltk-windows
WIN_FLTK_LIB := $(WIN_FLTK)/lib/libfltk.a
WIN_CXX      := $(WIN_PREFIX)g++
WIN_CC       := $(WIN_PREFIX)gcc
WIN_RC       := $(WIN_PREFIX)windres
WIN_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I$(WIN_FLTK)/include \
                -I$(MA_DIR) -I$(RTMIDI_DIR) -Isrc -isystem musil/src -DMORITZ_VERSION=\"$(VERSION)\" -D__WINDOWS_MM__ -DNOMINMAX \
                -DWIN32 -DUNICODE -D_UNICODE
WIN_OBJECTS  := $(patsubst src/%.cpp,$(WIN_BUILD)/%.o,$(SOURCES))
WIN_ZIP      := $(BUILD)/$(DIST_NAME)-windows-x86_64.zip

$(WIN_FLTK_LIB): $(FLTK_TGZ)
	@echo "==> building FLTK $(FLTK_VER) for Windows (static, MinGW-w64)"
	rm -rf $(WIN_FLTK_BLD) $(WIN_FLTK)
	[ -d $(FLTK_SRC) ] || tar -xzf $(FLTK_TGZ) -C $(DEPS)
	cmake -S $(FLTK_SRC) -B $(WIN_FLTK_BLD) \
	  $(WIN_CMAKE) \
	  -DCMAKE_C_COMPILER=$(WIN_CC) -DCMAKE_CXX_COMPILER=$(WIN_CXX) -DCMAKE_RC_COMPILER=$(WIN_RC) \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_INSTALL_PREFIX=$(abspath $(WIN_FLTK)) \
	  -DFLTK_BUILD_SHARED_LIBS=OFF \
	  -DFLTK_BUILD_GL=OFF \
	  -DFLTK_BUILD_TEST=OFF \
	  -DFLTK_BUILD_EXAMPLES=OFF \
	  -DFLTK_BUILD_FLUID=OFF \
	  -DFLTK_BUILD_FLTK_OPTIONS=OFF \
	  -DFLTK_USE_SYSTEM_LIBJPEG=OFF \
	  -DFLTK_USE_SYSTEM_LIBPNG=OFF \
	  -DFLTK_USE_SYSTEM_ZLIB=OFF
	cmake --build $(WIN_FLTK_BLD) --parallel
	cmake --install $(WIN_FLTK_BLD)
	@touch $@

$(WIN_BUILD)/miniaudio.o: $(MA_HDR)
	@mkdir -p $(WIN_BUILD)
	$(WIN_CC) -O2 -x c -DMINIAUDIO_IMPLEMENTATION -c $(MA_HDR) -o $@

$(WIN_BUILD)/RtMidi.o: $(RTMIDI_HDR)
	@mkdir -p $(WIN_BUILD)
	$(WIN_CXX) -std=c++17 -O2 -D__WINDOWS_MM__ -w -c $(RTMIDI_SRC) -o $@

$(WIN_BUILD)/resources.o: resources/moritz.rc resources/moritz.ico resources/moritz.manifest
	@mkdir -p $(WIN_BUILD)
	$(WIN_RC) -I resources resources/moritz.rc -O coff -o $@

$(WIN_BUILD)/%.o: src/%.cpp $(HEADERS) $(WIN_FLTK_LIB) $(MA_HDR) $(RTMIDI_HDR)
	@mkdir -p $(WIN_BUILD)
	$(WIN_CXX) $(WIN_CXXFLAGS) -c $< -o $@

$(WIN_DIR)/$(APP).exe: $(WIN_OBJECTS) $(WIN_BUILD)/miniaudio.o $(WIN_BUILD)/RtMidi.o $(WIN_BUILD)/resources.o
	@mkdir -p $(WIN_DIR)
	$(WIN_CXX) -mwindows -static -s -Wl,--stack,8388608 $^ $(WIN_FLTK)/lib/libfltk_images.a $(WIN_FLTK)/lib/libfltk_png.a \
	  $(WIN_FLTK)/lib/libfltk_z.a $(WIN_FLTK_LIB) -lwinmm -lole32 -luuid -lcomctl32 -lcomdlg32 \
	  -lgdi32 -lgdiplus -lwinspool -lws2_32 -lshell32 -ladvapi32 -limm32 -loleaut32 -o $@

WIN_MUSIL := $(WIN_BUILD)/musil/musil.exe
$(WIN_MUSIL): $(MUSIL_DEPS) $(WIN_FLTK_LIB)
	@mkdir -p $(dir $@)
	$(WIN_CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -DMUSIL_HAS_FLTK -I$(WIN_FLTK)/include -Imusil/src \
	  musil/cli/main.cpp -static -s -Wl,--stack,8388608 $(WIN_FLTK)/lib/libfltk_images.a $(WIN_FLTK)/lib/libfltk_png.a \
	  $(WIN_FLTK)/lib/libfltk_z.a $(WIN_FLTK_LIB) -lwinmm -lole32 -luuid -lcomctl32 -lcomdlg32 \
	  -lgdi32 -lgdiplus -lwinspool -lws2_32 -lshell32 -ladvapi32 -limm32 -loleaut32 -o $@

windows: $(WIN_DIR)/$(APP).exe $(WIN_MUSIL) $(SHARED)
	rm -rf $(WIN_DIR)/examples $(WIN_DIR)/library $(WIN_DIR)/musil $(WIN_DIR)/help $(WIN_DIR)/tutorials $(WIN_DIR)/resources
	rm -rf $(WIN_DIR)/docs $(WIN_DIR)/data
	cp -R examples library help tutorials LICENSE README.md OBJECTS.md $(WIN_DIR)/
	$(call copy_docs,$(WIN_DIR))
	$(call copy_musil,$(WIN_DIR))
	cp $(WIN_MUSIL) $(WIN_DIR)/musil/
	mkdir -p $(WIN_DIR)/resources && cp resources/about.png resources/maxundmoritz.png $(WIN_DIR)/resources/
	@echo "==> built $(WIN_DIR)/$(APP).exe"

windows-zip: windows
	rm -f $(WIN_ZIP)
	cd $(WIN_BUILD) && zip -qr -X ../$(notdir $(WIN_ZIP)) Moritz -x '*.DS_Store'
	@ls -l $(WIN_ZIP)

# ---------------------------------------------------------------- release (macOS)
# The zip holds the app (with the examples inside), the examples, the shared
# data and Musil on its own (musil/musil, its libraries and examples) as
# folders next to it, the documents, the README and the license. --norsrc leaves out
# resource forks and extended attributes, which would otherwise travel as
# ._ files and can invalidate the signature once unzipped on another Mac.
dist: $(BUNDLE) $(MUSIL_BIN)
	@echo "==> packaging $(DIST_ZIP)"
	rm -rf $(BUILD)/dist $(DIST_ZIP)
	mkdir -p $(BUILD)/dist/$(DIST_NAME)
	ditto $(BUNDLE) $(BUILD)/dist/$(DIST_NAME)/$(BUNDLE)
	ditto examples $(BUILD)/dist/$(DIST_NAME)/examples
	$(call copy_docs,$(BUILD)/dist/$(DIST_NAME))
	$(call copy_musil,$(BUILD)/dist/$(DIST_NAME))
	cp $(MUSIL_BIN) $(BUILD)/dist/$(DIST_NAME)/musil/
	cp LICENSE README.md OBJECTS.md $(BUILD)/dist/$(DIST_NAME)/
	find $(BUILD)/dist -name .DS_Store -delete
	xattr -cr $(BUILD)/dist
	codesign --verify --deep --strict $(BUILD)/dist/$(DIST_NAME)/$(BUNDLE)
	cd $(BUILD)/dist && ditto -c -k --norsrc --keepParent $(DIST_NAME) ../$(notdir $(DIST_ZIP))
	@ls -l $(DIST_ZIP)

notarize: $(BUNDLE)
	@if [ "$(SIGN_ID)" = "-" ]; then \
	  echo "notarize needs SIGN_ID=\"Developer ID Application: ...\" (see the top of this Makefile)"; \
	  exit 1; fi
	@echo "==> notarizing $(BUNDLE)"
	rm -f $(BUILD)/notarize.zip
	ditto -c -k --norsrc --keepParent $(BUNDLE) $(BUILD)/notarize.zip
	xcrun notarytool submit $(BUILD)/notarize.zip --keychain-profile "$(NOTARY_PROFILE)" --wait
	xcrun stapler staple $(BUNDLE)
	spctl --assess --type execute --verbose $(BUNDLE)
	$(MAKE) dist SIGN_ID="$(SIGN_ID)"

ifeq ($(UNAME),Linux)
run: linux
	$(LX_DIR)/$(APP)
else ifdef WINDOWS_HOST
run: windows
	$(WIN_DIR)/$(APP).exe
else
run: $(BUNDLE)
	open $(BUNDLE)
endif

ifeq ($(UNAME),Linux)
musil: $(LX_MUSIL)
MUSIL_HOST_BIN := $(LX_MUSIL)
else ifdef WINDOWS_HOST
musil: $(WIN_MUSIL)
MUSIL_HOST_BIN := $(WIN_MUSIL)
else
musil: $(MUSIL_BIN)
MUSIL_HOST_BIN := $(MUSIL_BIN)
endif

# Musil's tests on Musil itself (the reference), then on Musil in Moritz (moritz --run: no live, plot, controls)
test-musil: musil
	tools/test-musil.sh musil $(MUSIL_HOST_BIN)

# Musil inside another program: interpreters on threads, nothing left alive afterwards. Built from
# musil/ alone, so it also proves that Musil needs nothing of Moritz.
EMBED_CXX := $(CXX)
EMBED_LIBS := -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
ifeq ($(UNAME),Linux)
EMBED_LIBS := -pthread -ldl -lm
else ifdef WINDOWS_HOST
EMBED_CXX := $(WIN_CXX)
EMBED_LIBS := -static -lws2_32 -lole32 -lwinmm
endif
embed-test:
	mkdir -p $(BUILD)
	$(EMBED_CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Imusil/src musil/tests/embed.cpp -o $(BUILD)/embed-test $(EMBED_LIBS)
	MUSIL_PATH=musil/src $(BUILD)/embed-test

clean:
	rm -rf $(BUILD) $(APP) $(BUNDLE)

distclean: clean
	rm -rf $(DEPS)

.PHONY: all deps run musil test-musil embed-test linux linux-dist windows windows-zip dist notarize clean distclean
