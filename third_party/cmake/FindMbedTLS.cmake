# In-tree override of FindMbedTLS.cmake
#
# The mbedtls subdirectory already created the targets:
#   MbedTLS::mbedcrypto  (alias for mbedcrypto)
#   MbedTLS::mbedx509    (alias for mbedx509)
#   MbedTLS::mbedtls     (alias for mbedtls)
#
# This module makes find_package(MbedTLS) succeed immediately
# without searching the filesystem for built artifacts.

if(TARGET MbedTLS::mbedcrypto)
    set(MBEDTLS_FOUND TRUE)
    get_target_property(_mbedtls_inc MbedTLS::mbedcrypto INTERFACE_INCLUDE_DIRECTORIES)
    set(MBEDTLS_INCLUDE_DIR "${_mbedtls_inc}")
    set(MBEDTLS_INCLUDE_DIRS "${_mbedtls_inc}")
    set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)
    unset(_mbedtls_inc)
    return()
endif()

# Fallback: search on disk (should not be needed when mbedtls is a subdirectory)
message(STATUS "FindMbedTLS: MbedTLS::mbedcrypto target not found, falling back to file search")
