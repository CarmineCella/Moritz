# Moritz — a minimal Max/MSP-like patcher
#
#   make          download + build deps (first run only), then build:
#                 on macOS Moritz.app, on Linux build/linux/Moritz/ (= make linux)
#   make run      build and open the app
#   make linux    the Linux build: build/linux/Moritz/moritz with the examples,
#                 library and help next to it (needs the packages listed below)
#   make dist     macOS: build the release zip (app + examples + license) in build/
#   make notarize macOS: sign with a Developer ID, notarize and staple (see below)
#   make clean    remove build products
#   make distclean  also remove downloaded dependencies
#
# On macOS the app is a universal (arm64 + x86_64) binary statically linked
# with FLTK. Dependencies are downloaded into deps/ on the first build: FLTK
# (built with cmake), miniaudio (audio) and RtMidi (MIDI).
#
# Linux packages (Debian/Ubuntu names): build-essential cmake curl
#   libasound2-dev libx11-dev libxext-dev libxft-dev libxinerama-dev
#   libxcursor-dev libxrender-dev libxfixes-dev libfontconfig1-dev

APP        := moritz
BUNDLE     := Moritz.app
VERSION    := 0.4
UNAME      := $(shell uname -s)

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
              -I$(FLTK_INST)/include -I$(MA_DIR) -I$(RTMIDI_DIR) -Isrc -DMORITZ_VERSION=\"$(VERSION)\"
MAFLAGS    := -O2 $(ARCHFLAGS) -DMA_NO_RUNTIME_LINKING
FRAMEWORKS := -framework Cocoa -framework CoreAudio -framework AudioToolbox -framework CoreMIDI \
              -framework CoreFoundation -weak_framework UniformTypeIdentifiers
LDFLAGS    := $(ARCHFLAGS)

ifeq ($(UNAME),Linux)
all: linux
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

SOURCES    := $(wildcard src/*.cpp)
OBJECTS    := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SOURCES))
HEADERS    := $(wildcard src/*.h)

$(BUILD)/%.o: src/%.cpp $(HEADERS) $(FLTK_LIB) $(MA_HDR) $(RTMIDI_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -D__MACOSX_CORE__ -c $< -o $@

$(APP): $(OBJECTS) $(BUILD)/miniaudio.o $(BUILD)/RtMidi.o
	$(CXX) $(LDFLAGS) $^ $(FLTK_LIBS) $(FRAMEWORKS) -o $@

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

$(BUNDLE): $(APP) $(wildcard examples/*.moritz examples/sounds/* library/* help/* resources/*) LICENSE
	@echo "==> creating $(BUNDLE)"
	rm -rf $(BUNDLE)
	mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources
	cp $(APP) $(BUNDLE)/Contents/MacOS/$(APP)
	cp -R examples library help $(BUNDLE)/Contents/Resources/ 2>/dev/null || true
	cp resources/moritz.icns resources/about.png LICENSE $(BUNDLE)/Contents/Resources/
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
               -I$(MA_DIR) -I$(RTMIDI_DIR) -Isrc -DMORITZ_VERSION=\"$(VERSION)\" -D__LINUX_ALSA__
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

linux: $(LX_DIR)/$(APP) $(wildcard examples/*.moritz examples/sounds/* library/* help/*)
	rm -rf $(LX_DIR)/examples $(LX_DIR)/library $(LX_DIR)/help
	cp -R examples library help LICENSE README.md $(LX_DIR)/
	@echo "==> built $(LX_DIR)/$(APP)"

# ---------------------------------------------------------------- release (macOS)
# The zip holds the app (with the examples inside), the examples as a
# folder next to it, the README and the license. --norsrc leaves out
# resource forks and extended attributes, which would otherwise travel as
# ._ files and can invalidate the signature once unzipped on another Mac.
dist: $(BUNDLE)
	@echo "==> packaging $(DIST_ZIP)"
	rm -rf $(BUILD)/dist $(DIST_ZIP)
	mkdir -p $(BUILD)/dist/$(DIST_NAME)
	ditto $(BUNDLE) $(BUILD)/dist/$(DIST_NAME)/$(BUNDLE)
	ditto examples $(BUILD)/dist/$(DIST_NAME)/examples
	cp LICENSE README.md $(BUILD)/dist/$(DIST_NAME)/
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
else
run: $(BUNDLE)
	open $(BUNDLE)
endif

clean:
	rm -rf $(BUILD) $(APP) $(BUNDLE)

distclean: clean
	rm -rf $(DEPS)

.PHONY: all deps run linux dist notarize clean distclean
