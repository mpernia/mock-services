// Project-specific Mbed TLS configuration overlay.
// Included after the upstream mbedtls_config.h through MBEDTLS_USER_CONFIG_FILE.

#ifndef MOCK_SERVICES_MBEDTLS_USER_CONFIG_H
#define MOCK_SERVICES_MBEDTLS_USER_CONFIG_H

#ifndef MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_C
#endif

#ifndef MBEDTLS_THREADING_PTHREAD
#define MBEDTLS_THREADING_PTHREAD
#endif

#endif  // MOCK_SERVICES_MBEDTLS_USER_CONFIG_H
