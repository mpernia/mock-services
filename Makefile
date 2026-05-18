.PHONY: all build test test-sftp clean distclean mrproper \
        examples examples-sftp rebuild \
        install release

# ── Detect OS ──────────────────────────────────────────────────────────

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),)
$(error Cannot detect the operating system)
endif

IS_LINUX   := $(filter Linux,$(UNAME_S))
IS_MINGW   := $(findstring MINGW,$(UNAME_S))
IS_MSYS    := $(findstring MSYS,$(UNAME_S))
IS_WINDOWS := $(or $(IS_MINGW),$(IS_MSYS))

# ── OS-aware defaults ─────────────────────────────────────────────────
# The Makefile is a thin convenience layer over Conan + CMake, both of
# which handle OS detection transparently. We just pick sensible defaults
# for known platforms so you don't have to type PRESET=... every time.
# Unknown OS → no defaults set; CMake will error with a clear message.
# Override any var:  make build PRESET=conan-debug BUILD_DIR=build/Debug

ifeq ($(IS_LINUX),Linux)

PRESET     ?= conan-release
BUILD_DIR  ?= build/Release
PREFIX     ?= /usr/local

else ifneq ($(IS_WINDOWS),)

PRESET     ?= conan-mingw32-debug
BUILD_DIR  ?= build/mingw32/Debug
PREFIX     ?= $(LOCALAPPDATA)/mock-services

endif

SFTP       ?= OFF
CTEST_ARGS ?= --output-on-failure
RELEASE_DIR ?= dist/mock-services

# ── Conan wrapper ───────────────────────────────────────────────────────

CONAN := python -c "from conans.conan import run; import sys; sys.exit(run())"

# ── All (build + test) ───────────────────────────────────────────────────

all: build test

# ── Build ────────────────────────────────────────────────────────────────

build:
	@if [ ! -f "$(BUILD_DIR)/generators/CMakePresets.json" ]; then \
		echo "Conan generators not found — running conan install + cmake configure..."; \
		conan install . --output-folder="." --build=missing && \
		cmake --preset $(PRESET); \
	fi
	cmake --build --preset $(PRESET)

rebuild: distclean
	$(MAKE) build

# ── Examples ─────────────────────────────────────────────────────────────

examples:
	cmake --build --preset $(PRESET) --target rest_example
	cmake --build --preset $(PRESET) --target soap_example
	cmake --build --preset $(PRESET) --target ftp_example
	cmake --build --preset $(PRESET) --target mqtt_example

examples-sftp:
	$(MAKE) examples
	cmake --build --preset $(PRESET) --target sftp_example

# ── Test ─────────────────────────────────────────────────────────────────

test:
	ctest --preset $(PRESET) $(CTEST_ARGS)

test-sftp:
	ctest --test-dir $(BUILD_DIR) $(CTEST_ARGS)

# ── Install ──────────────────────────────────────────────────────────────

install:
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)

# ── Release (build + package for distribution) ──────────────────────────

RELEASE_ARCH := $(shell uname -m)

release:
	@echo "Packaging release into $(RELEASE_DIR)/ ..."
	@mkdir -p $(RELEASE_DIR)/lib $(RELEASE_DIR)/include
	# Build with SFTP enabled (requires FetchContent downloads libssh + mbedtls)
	conan install . --output-folder="." --build=missing -o "&:with_sftp=True" && \
	cmake --preset $(PRESET) -DMOCK_SERVICES_ENABLE_SFTP=ON && \
	cmake --build --preset $(PRESET)
	cp -a $(BUILD_DIR)/libmock-services.a $(RELEASE_DIR)/lib/
	strip --strip-debug $(RELEASE_DIR)/lib/libmock-services.a
	cp -a include/mock-services $(RELEASE_DIR)/include/
	# Bundle pugixml (headers + static lib)
	PUGIXML_DIR=$$(grep '^set(pugixml_PACKAGE_FOLDER_RELEASE' \
		$(BUILD_DIR)/generators/pugixml-release-x86_64-data.cmake 2>/dev/null \
		| sed 's/.*"\(.*\)")$$/\1/'); \
	if [ -n "$$PUGIXML_DIR" ]; then \
		cp -a "$$PUGIXML_DIR/include/"* $(RELEASE_DIR)/include/; \
		cp -a "$$PUGIXML_DIR/lib/"* $(RELEASE_DIR)/lib/; \
		strip --strip-debug $(RELEASE_DIR)/lib/libpugixml.a 2>/dev/null || true; \
	fi
	# Bundle cpp-httplib headers (header-only)
	HTTPLIB_DIR=$$(grep '^set(cpp-httplib_PACKAGE_FOLDER_RELEASE' \
		$(BUILD_DIR)/generators/httplib-release-x86_64-data.cmake 2>/dev/null \
		| sed 's/.*"\(.*\)")$$/\1/'); \
	if [ -n "$$HTTPLIB_DIR" ]; then \
		cp -a "$$HTTPLIB_DIR/include/"* $(RELEASE_DIR)/include/; \
	fi
	# Bundle SFTP dependencies (libssh + mbedtls), only when SFTP is enabled
	if [ -d "$(BUILD_DIR)/_deps/libssh-build" ]; then \
		mkdir -p $(RELEASE_DIR)/include/libssh $(RELEASE_DIR)/include/mbedtls $(RELEASE_DIR)/include/psa; \
		cp -a $(BUILD_DIR)/_deps/libssh-src/include/libssh/*.h $(RELEASE_DIR)/include/libssh/; \
		cp -a $(BUILD_DIR)/_deps/mbedtls-src/include/mbedtls/*.h $(RELEASE_DIR)/include/mbedtls/; \
		cp -a $(BUILD_DIR)/_deps/mbedtls-src/include/psa/*.h $(RELEASE_DIR)/include/psa/; \
		cp -a $(BUILD_DIR)/_deps/libssh-build/src/libssh.a $(RELEASE_DIR)/lib/; \
		cp -a $(BUILD_DIR)/_deps/mbedtls-build/library/libmbedtls.a $(RELEASE_DIR)/lib/; \
		cp -a $(BUILD_DIR)/_deps/mbedtls-build/library/libmbedcrypto.a $(RELEASE_DIR)/lib/; \
		cp -a $(BUILD_DIR)/_deps/mbedtls-build/library/libmbedx509.a $(RELEASE_DIR)/lib/; \
		cp -a $(BUILD_DIR)/_deps/mbedtls-build/3rdparty/everest/libeverest.a $(RELEASE_DIR)/lib/ 2>/dev/null || true; \
		cp -a $(BUILD_DIR)/_deps/mbedtls-build/3rdparty/p256-m/libp256m.a $(RELEASE_DIR)/lib/ 2>/dev/null || true; \
		strip --strip-debug $(RELEASE_DIR)/lib/libssh.a 2>/dev/null || true; \
	fi
	cp LICENSE $(RELEASE_DIR)/
	RELEASE_TAG=$$(git describe --tags --always 2>/dev/null || echo "dev"); \
	if [ -n "$(IS_LINUX)" ]; then \
		cd dist && tar czf mock-services-$$RELEASE_TAG-linux-$(RELEASE_ARCH).tar.gz mock-services/; \
	else \
		cd dist && cmake -E tar cf mock-services-$$RELEASE_TAG-win32.zip --format=zip mock-services/; \
	fi
	@echo "Release package created: dist/mock-services-*"

# ── Clean ────────────────────────────────────────────────────────────────

clean:
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		cmake --build --preset $(PRESET) --target clean; \
	fi
	rm -rf $(RELEASE_DIR) dist/*.tar.gz dist/*.zip

distclean:
	rm -rf build/
	rm -f CMakeUserPresets.json compile_commands.json

mrproper: distclean
	rm -rf third_party/mbedtls/ third_party/libssh/
	rm -rf Testing/ dist/
	rm -rf .cache/

# ── Help ─────────────────────────────────────────────────────────────────

help:
	@echo "mock-services Makefile"
	@echo ""
	@echo "Usage:"
	@echo "  make              Build + test (default)"
	@echo "  make build        Build library + tests"
	@echo "  make test         Run tests"
	@echo "  make examples     Build example binaries (REST, SOAP, FTP, MQTT)"
	@echo "  make install      Install to PREFIX=$(PREFIX)"
	@echo "  make release      Build Release + package tarball into dist/"
	@echo "  make clean        Remove compiled objects (CMake --target clean)"
	@echo "  make distclean    Remove entire build/ directory"
	@echo "  make mrproper     Remove build/ + third_party/ + test output"
	@echo ""
	@echo "Platform: $(UNAME_S)"
	@echo "Variables:"
	@echo "  PRESET=$(PRESET)        CMake preset to use"
	@echo "  BUILD_DIR=$(BUILD_DIR)  Build output directory"
	@echo "  PREFIX=$(PREFIX)        Install prefix"
	@echo "  SFTP=$(SFTP)            Set to ON to enable SFTP"
	@echo ""
	@echo "Presets available:"
	@cmake --list-presets 2>/dev/null | tail -n +3 || echo "  (none configured)"
