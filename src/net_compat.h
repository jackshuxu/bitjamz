#pragma once
// Cross-platform socket shim. Include this instead of the raw POSIX socket
// headers. Provides create_socket(), accept_socket(), close_socket(), and
// net_init()/net_cleanup() that are no-ops on POSIX.
// Also provides bitjams_ctz() — a portable trailing-zero-count replacing
// __builtin_ctz (not available on MSVC).

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <intrin.h>
// Map the POSIX shutdown constant used throughout the codebase.
#  define SHUT_RDWR SD_BOTH
// socket() and accept() return SOCKET (uintptr_t) on Windows; a `< 0` guard
// is UB on an unsigned type, so wrap them to return -1 on failure.
inline int create_socket() {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    return (s == INVALID_SOCKET) ? -1 : static_cast<int>(s);
}
inline int accept_socket(int fd, sockaddr* a, socklen_t* l) {
    SOCKET s = ::accept(static_cast<SOCKET>(fd), a, l);
    return (s == INVALID_SOCKET) ? -1 : static_cast<int>(s);
}
inline void close_socket(int fd) { ::closesocket(static_cast<SOCKET>(fd)); }
inline bool net_init()    { WSADATA w; return ::WSAStartup(MAKEWORD(2, 2), &w) == 0; }
inline void net_cleanup() { ::WSACleanup(); }
inline int bitjams_ctz(unsigned int x) {
    unsigned long idx;
    _BitScanForward(&idx, static_cast<unsigned long>(x));
    return static_cast<int>(idx);
}

#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
inline int  create_socket()                                  { return ::socket(AF_INET, SOCK_STREAM, 0); }
inline int  accept_socket(int fd, sockaddr* a, socklen_t* l) { return ::accept(fd, a, l); }
inline void close_socket(int fd)                             { ::close(fd); }
inline bool net_init()                                       { return true; }
inline void net_cleanup()                                    {}
inline int  bitjams_ctz(unsigned int x)                      { return __builtin_ctz(x); }
#endif
