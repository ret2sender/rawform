# rawform - convenience wrapper around CMake.
#
# This Makefile does not build anything itself; it drives CMake so the common
# workflows are one word long. CMake remains the source of truth, and CLion
# users can ignore this file entirely.
#
#   make                    build the application (Debug)
#   make -j6                same, six parallel jobs
#   make BUILD_TYPE=Release
#   make run                build, then run with logging attached to the terminal
#   make core               build the audio engine, CLI and tests standalone
#   make test               run the engine unit tests
#   make test SANITIZE=thread   the concurrency soaks under TSan
#   make bundle             make the .app self-contained (macOS)
#   make dmg                self-contained .app plus a .dmg (macOS)
#   make install            copy the bundle to /Applications (macOS)
#   make clean              remove build artifacts, keep the CMake configuration
#   make distclean          remove the build directory entirely
#   make help               list targets
#
# This file is part of rawform.
# Copyright (C) 2026 Etienne Fleurant
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

# ---------------------------------------------------------------------------
# Configuration - override on the command line, e.g. make BUILD_TYPE=Release
#
# Each build type gets its own directory. CMAKE_BUILD_TYPE is baked into the
# CMake cache at configure time, so sharing one directory between Debug and
# Release would silently keep whichever was configured first.
# ---------------------------------------------------------------------------
BUILD_TYPE     ?= Debug
BUILD_TYPE_LC  := $(shell echo $(BUILD_TYPE) | tr '[:upper:]' '[:lower:]')
APP_BUILD_DIR  ?= build/app-$(BUILD_TYPE_LC)
GENERATOR      ?= Unix Makefiles

# FFmpeg fallback decoder, forwarded to the engine's tri-state option:
#   auto (default)  probe quietly; enable iff the four libav* libs are found
#   on              require them, hard error if missing
#   off             native-only build, never probe
RAWFORM_FFMPEG ?= auto

# Sanitizer profile for the standalone engine build (make core / make test),
# forwarded to the engine's RAWFORM_SANITIZE:
#   off (default) | thread (TSan) | address (ASan+UBSan)
# Each profile gets its own build directory, for the same cache-baking
# reason as BUILD_TYPE.
SANITIZE       ?= off
ifeq ($(SANITIZE),off)
CORE_BUILD_DIR ?= build/core-$(BUILD_TYPE_LC)
else
CORE_BUILD_DIR ?= build/core-$(BUILD_TYPE_LC)-$(SANITIZE)
endif

# ---------------------------------------------------------------------------
# Qt discovery
#
# Qt has no standard install location on macOS, so CMake must be told where it
# is. QT_PREFIX is resolved in this order:
#
#   1. whatever is passed on the command line or set in the environment
#   2. Homebrew's qt formula
#   3. the Qt online installer's default location (~/Qt/6.x.x/macos), newest
#      version first
#
# A candidate only counts if it actually contains Qt6Config.cmake: `brew
# --prefix qt` prints a path whether or not the formula is installed, so the
# path alone proves nothing (the scenario this prevents: a machine that uses
# the online installer and has no brew qt formula still gets a brew path).
# ---------------------------------------------------------------------------
QT_PREFIX ?=

ifeq ($(strip $(QT_PREFIX)),)
QT_PREFIX := $(shell brew --prefix qt 2>/dev/null)
endif
ifeq ($(wildcard $(QT_PREFIX)/lib/cmake/Qt6/Qt6Config.cmake),)
# Online-installer layouts on both platforms: macos on the Mac,
# gcc_64 on Linux. The sort picks the newest version; the Qt6Config.cmake
# check below still decides, so a plausible-but-empty path proves nothing on
# either leg.
QT_PREFIX := $(lastword $(sort $(wildcard $(HOME)/Qt/6.*/macos $(HOME)/Qt/6.*/gcc_64)))
endif

QT_CONFIG := $(wildcard $(QT_PREFIX)/lib/cmake/Qt6/Qt6Config.cmake)

APP_BUNDLE := $(APP_BUILD_DIR)/rawform.app
APP_BINARY := $(APP_BUNDLE)/Contents/MacOS/rawform

# On Linux the target is a plain executable rather than a bundle.
UNAME_S := $(shell uname -s)
ifneq ($(UNAME_S),Darwin)
APP_BINARY := $(APP_BUILD_DIR)/rawform
endif

CMAKE_COMMON := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DRAWFORM_FFMPEG=$(RAWFORM_FFMPEG)

CMAKE_APP_FLAGS := $(CMAKE_COMMON)
ifneq ($(strip $(QT_PREFIX)),)
CMAKE_APP_FLAGS += -DCMAKE_PREFIX_PATH=$(QT_PREFIX)
endif

CMAKE_CORE_FLAGS := $(CMAKE_COMMON) -DRAWFORM_SANITIZE=$(SANITIZE)

.DEFAULT_GOAL := app
.PHONY: all app core check-qt check-deps configure configure-core run test \
        bundle dmg install clean distclean help

all: app

# ---------------------------------------------------------------------------
# Qt preflight
#
# Without this, a missing Qt surfaces much later as an opaque
# "Could not find a package configuration file provided by Qt6" from CMake.
# ---------------------------------------------------------------------------
check-qt:
	@if [ -z "$(strip $(QT_PREFIX))" ] || [ -z "$(strip $(QT_CONFIG))" ]; then \
		echo "error: Qt 6 not found."; \
		echo; \
		if [ -n "$(strip $(QT_PREFIX))" ]; then \
			echo "  Looked in : $(QT_PREFIX)"; \
			echo "  Expected  : $(QT_PREFIX)/lib/cmake/Qt6/Qt6Config.cmake"; \
			echo; \
		fi; \
		echo "  Install Qt with the online installer (https://www.qt.io/download-qt-installer)"; \
		echo "  or via Homebrew:"; \
		echo "      brew install qt"; \
		echo; \
		echo "  Or point this build at an existing installation:"; \
		echo "      make QT_PREFIX=\$$HOME/Qt/6.10.0/macos"; \
		echo "      make QT_PREFIX=\"\$$(brew --prefix qt)\""; \
		echo; \
		exit 1; \
	fi

# ---------------------------------------------------------------------------
# Audio / metadata dependency preflight
#
# Every library rawform links from Homebrew or a Linux distro ships a
# pkg-config file, so one uniform probe covers them all. Without this, a
# missing codec library surfaces later as a CMake error naming a target the
# reader has to map back to a formula. libebur128's .pc module is named
# libebur128 on most installs and ebur128 on a few, so both are probed
# (mirroring the CMake discovery).
#
# FFmpeg is only REQUIRED here when RAWFORM_FFMPEG=on; auto is the engine's
# probe-quietly mode and gets a status line, not a failure.
#
# If pkg-config itself is absent the checks are skipped with a warning rather
# than failed: CMake's CONFIG-mode discovery can still succeed without it,
# and a false failure would be worse than a late one.
# ---------------------------------------------------------------------------
check-deps:
	@if ! command -v pkg-config >/dev/null 2>&1; then \
		echo "warning: pkg-config not found; skipping dependency preflight."; \
		echo "         (brew install pkg-config to enable it)"; \
	else \
		missing=""; \
		pkg-config --exists sndfile      || missing="$$missing libsndfile"; \
		pkg-config --exists flac         || missing="$$missing flac"; \
		pkg-config --exists libmpg123    || missing="$$missing mpg123"; \
		pkg-config --exists libebur128 || pkg-config --exists ebur128 \
		                                 || missing="$$missing libebur128"; \
		pkg-config --exists yaml-cpp     || missing="$$missing yaml-cpp"; \
		pkg-config --atleast-version=2.0 taglib \
		                                 || missing="$$missing taglib"; \
		if [ "$(RAWFORM_FFMPEG)" = "on" ]; then \
			pkg-config --exists libavformat libavcodec libavutil libswresample \
			                             || missing="$$missing ffmpeg"; \
		fi; \
		if [ -n "$$missing" ]; then \
			echo "error: missing dependencies:$$missing"; \
			echo; \
			echo "  Install them with:"; \
			echo "      brew install$$missing        (macOS)"; \
			echo "  or your distro's -dev packages   (Linux)"; \
			echo; \
			echo "  Note: taglib must be 2.x (the 2.0 CMake config package)."; \
			exit 1; \
		fi; \
		if [ "$(RAWFORM_FFMPEG)" = "auto" ]; then \
			if pkg-config --exists libavformat libavcodec libavutil libswresample; then \
				echo "FFmpeg fallback: will be ENABLED (RAWFORM_FFMPEG=auto, libs found)"; \
			else \
				echo "FFmpeg fallback: will be disabled (RAWFORM_FFMPEG=auto, libs not found)"; \
			fi; \
		fi; \
	fi

# ---------------------------------------------------------------------------
# Application (builds the engine too, via add_subdirectory)
#
# Note: the configure rule only fires when the build directory has no
# Makefile yet. To change a cached option (e.g. RAWFORM_FFMPEG) in an
# existing directory, run `make configure RAWFORM_FFMPEG=off` explicitly.
# ---------------------------------------------------------------------------
$(APP_BUILD_DIR)/Makefile: | check-qt check-deps
	cmake -S ui -B $(APP_BUILD_DIR) -G "$(GENERATOR)" $(CMAKE_APP_FLAGS)

configure: check-qt check-deps
	cmake -S ui -B $(APP_BUILD_DIR) -G "$(GENERATOR)" $(CMAKE_APP_FLAGS)

# Recursive make so that -jN reaches the real build through the jobserver.
app: $(APP_BUILD_DIR)/Makefile
	+$(MAKE) -C $(APP_BUILD_DIR)

run: app
	$(APP_BINARY)

# ---------------------------------------------------------------------------
# Audio engine, CLI and unit tests, built standalone
#
# The engine's test targets are configured but not built by the application
# build (EXCLUDE_FROM_ALL), which is why they live here.
# ---------------------------------------------------------------------------
$(CORE_BUILD_DIR)/Makefile: | check-deps
	cmake -S audio -B $(CORE_BUILD_DIR) -G "$(GENERATOR)" $(CMAKE_CORE_FLAGS)

configure-core: check-deps
	cmake -S audio -B $(CORE_BUILD_DIR) -G "$(GENERATOR)" $(CMAKE_CORE_FLAGS)

core: $(CORE_BUILD_DIR)/Makefile
	+$(MAKE) -C $(CORE_BUILD_DIR)

test: core
	ctest --test-dir $(CORE_BUILD_DIR) --output-on-failure

# ---------------------------------------------------------------------------
# macOS packaging
# ---------------------------------------------------------------------------
bundle: app
	QT_PREFIX="$(QT_PREFIX)" ./scripts/macdeploy.sh $(APP_BUNDLE)

# BUILD_TYPE is forwarded so the .dmg name reflects what is inside it:
# 'make dmg' (Debug by default) yields ...-debug.dmg, while
# 'make BUILD_TYPE=Release dmg' yields the clean release name.
dmg: app
	QT_PREFIX="$(QT_PREFIX)" BUILD_TYPE="$(BUILD_TYPE)" ./scripts/macdeploy.sh $(APP_BUNDLE) --dmg

install: bundle
	rm -rf /Applications/rawform.app
	cp -R $(APP_BUNDLE) /Applications/
	@echo "Installed /Applications/rawform.app"

# ---------------------------------------------------------------------------
# Cleaning
# ---------------------------------------------------------------------------
clean:
	@if [ -f $(APP_BUILD_DIR)/Makefile ]; then \
		$(MAKE) -C $(APP_BUILD_DIR) clean; \
	else \
		echo "nothing to clean in $(APP_BUILD_DIR)"; \
	fi
	@if [ -f $(CORE_BUILD_DIR)/Makefile ]; then \
		$(MAKE) -C $(CORE_BUILD_DIR) clean; \
	else \
		echo "nothing to clean in $(CORE_BUILD_DIR)"; \
	fi

distclean:
	rm -rf build

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------
help:
	@echo "rawform - available targets"
	@echo
	@echo "  app         Build the application (default). Builds the engine too."
	@echo "  run         Build, then run with logging on the terminal."
	@echo "  core        Build the audio engine, CLI and tests standalone."
	@echo "  test        Run the engine unit tests via ctest."
	@echo "  bundle      macOS: make the .app self-contained."
	@echo "  dmg         macOS: self-contained .app plus a .dmg."
	@echo "  install     macOS: copy the bundle to /Applications."
	@echo "  configure   Re-run CMake for the application (applies changed options)."
	@echo "  check-deps  Preflight the audio/metadata dependencies only."
	@echo "  clean       Remove build artifacts, keep the CMake configuration."
	@echo "  distclean   Remove the build directory entirely."
	@echo
	@echo "Common workflows:"
	@echo "  make -j6                          development build"
	@echo "  make run                          build and run with logging"
	@echo "  make test                         engine unit tests"
	@echo "  make test SANITIZE=thread         concurrency soaks under TSan"
	@echo "  make configure RAWFORM_FFMPEG=off reconfigure native-only"
	@echo "  make BUILD_TYPE=Release dmg       distributable disk image"
	@echo "  make BUILD_TYPE=Release install   install to /Applications"
	@echo
	@echo "Variables (override on the command line):"
	@echo "  BUILD_TYPE=$(BUILD_TYPE)"
	@echo "  RAWFORM_FFMPEG=$(RAWFORM_FFMPEG)"
	@echo "  SANITIZE=$(SANITIZE)"
	@echo "  APP_BUILD_DIR=$(APP_BUILD_DIR)"
	@echo "  CORE_BUILD_DIR=$(CORE_BUILD_DIR)"
	@echo "  GENERATOR=$(GENERATOR)"
	@echo "  QT_PREFIX=$(QT_PREFIX)"
