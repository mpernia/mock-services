#ifndef MOCK_SERVICES_DETAIL_PLATFORM_SOCKET_H
#define MOCK_SERVICES_DETAIL_PLATFORM_SOCKET_H

#include <stdexcept>

#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace mock_services {

#ifdef _WIN32
using raw_socket = SOCKET;
const raw_socket kInvalidSock = INVALID_SOCKET;
const int kSockErr = SOCKET_ERROR;

inline void sock_shutdown(raw_socket s) { ::shutdown(s, SD_BOTH); }
inline void sock_close(raw_socket s) { closesocket(s); }

inline void wsa_init() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        throw std::runtime_error("WSAStartup failed");
}
inline void wsa_fini() { WSACleanup(); }

#else
using raw_socket = int;
const raw_socket kInvalidSock = -1;
const int kSockErr = -1;

inline void sock_shutdown(raw_socket s) { ::shutdown(s, SHUT_RDWR); }
inline void sock_close(raw_socket s) { close(s); }

inline void wsa_init() {}
inline void wsa_fini() {}
#endif

inline void set_reuseaddr(raw_socket socket) {
    int opt = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
}

}
#endif
