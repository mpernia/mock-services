# mock-services third-party dependencies (mbedtls + libssh)
#
# Downloaded at configure-time via FetchContent.
# Built as static libraries for SFTP server support.
# Used only when MOCK_SERVICES_ENABLE_SFTP=ON.

cmake_minimum_required(VERSION 3.15)

include(FetchContent)

# Threading is needed by mbedtls (MBEDTLS_THREADING_PTHREAD) and libssh
find_package(Threads REQUIRED)

# ── Mbed TLS 3.6.0 ──────────────────────────────────────────────────────────
# Build as static library with pthread linking enabled.
# Keep upstream mbedtls_config.h untouched; project-specific options are
# appended through MBEDTLS_USER_CONFIG_FILE.

set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(LINK_WITH_PTHREAD          ON  CACHE BOOL "" FORCE)
set(ENABLE_TESTING             OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS            OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls_user_config.h"
    CACHE FILEPATH "" FORCE)

FetchContent_Declare(mbedtls
    GIT_REPOSITORY  https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG         v3.6.0
    GIT_SHALLOW     TRUE
    GIT_SUBMODULES  "framework"  # required by mbedtls 3.6's CMakeLists.txt
    TLS_VERIFY      OFF          # work around Windows Schannel SEC_E_DECRYPT_FAILURE
)

FetchContent_MakeAvailable(mbedtls)

# Ensure mbedtls targets are in an export set so CMake generation doesn't
# fail when processing libssh's install(EXPORT ...) commands. The actual
# install is never executed (SKIP_LIBSSH_INSTALL=ON), but CMake validates
# the export set at generate time regardless.
install(TARGETS mbedcrypto mbedx509 mbedtls p256m everest EXPORT libssh-config)

# ── libssh 0.12.0 ───────────────────────────────────────────────────────────
# Use mbedtls as crypto backend. Disable everything not needed.

# Point find_package(MbedTLS) at our in-tree alias override.
# Must be set BEFORE FetchContent_MakeAvailable(libssh) so libssh's
# subdirectory picks it up from CMAKE_MODULE_PATH.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(WITH_MBEDTLS       ON  CACHE BOOL "" FORCE)
set(WITH_OPENSSL       OFF CACHE BOOL "" FORCE)
set(WITH_GCRYPT        OFF CACHE BOOL "" FORCE)
set(WITH_GSSAPI        OFF CACHE BOOL "" FORCE)
set(WITH_ZLIB          OFF CACHE BOOL "" FORCE)
set(WITH_NACL          OFF CACHE BOOL "" FORCE)
set(WITH_FIDO2         OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(WITH_PCAP          OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARKS    OFF CACHE BOOL "" FORCE)
set(WITH_SFTP          ON  CACHE BOOL "" FORCE)
set(WITH_SERVER        ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
set(UNIT_TESTING       OFF CACHE BOOL "" FORCE)
set(CLIENT_TESTING     OFF CACHE BOOL "" FORCE)
set(SERVER_TESTING     OFF CACHE BOOL "" FORCE)

if(NOT UNIX)
    set(WITH_SYMBOL_VERSIONING OFF CACHE BOOL "" FORCE)
endif()

set(SKIP_LIBSSH_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(libssh
    GIT_REPOSITORY  https://git.libssh.org/projects/libssh.git
    GIT_TAG         libssh-0.12.0
    GIT_SHALLOW     TRUE
)

FetchContent_MakeAvailable(libssh)
