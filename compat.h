// compat.h — abstracciones Linux / Windows (MinGW-w64) para rat-lab.
// En Linux no cambia nada funcional.
// En Windows: compilar con MinGW-w64, enlazar con -lws2_32.
#ifndef RATLAB_COMPAT_H
#define RATLAB_COMPAT_H

#ifdef _WIN32
#  include <winsock2.h>   /* siempre antes que windows.h */
#  include <ws2tcpip.h>   /* inet_ntop, socklen_t, INET_ADDRSTRLEN */
#  include <windows.h>
#  include <tlhelp32.h>   /* Process32First / Process32Next */
#  include <io.h>
#  include <process.h>

   /* ssize_t no existe en MSVC; MinGW suele definirla, pero por si acaso */
#  ifndef _SSIZE_T_DEFINED
     typedef int ssize_t;
#    define _SSIZE_T_DEFINED
#  endif

   /* Winsock usa send/recv; write/read son para ficheros, no sockets */
   static inline ssize_t rl_sock_write(int fd, const void *b, size_t n) {
       return (ssize_t)send((SOCKET)(uintptr_t)fd, (const char *)b, (int)n, 0);
   }
   static inline ssize_t rl_sock_read(int fd, void *b, size_t n) {
       return (ssize_t)recv((SOCKET)(uintptr_t)fd, (char *)b, (int)n, 0);
   }
#  define RL_CLOSE_SOCKET(s) closesocket((SOCKET)(uintptr_t)(s))

   /* popen/pclose */
#  define popen  _popen
#  define pclose _pclose

   /* Inicialización de red */
   static inline int rl_net_init(void) {
       WSADATA w;
       return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
   }
   static inline void rl_net_cleanup(void) { WSACleanup(); }

#else  /* Linux / macOS / POSIX */
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>

   static inline ssize_t rl_sock_write(int fd, const void *b, size_t n) {
       return write(fd, b, n);
   }
   static inline ssize_t rl_sock_read(int fd, void *b, size_t n) {
       return read(fd, b, n);
   }
#  define RL_CLOSE_SOCKET(s) close(s)

   static inline int  rl_net_init(void)    { return 0; }
   static inline void rl_net_cleanup(void) { }
#endif

#endif /* RATLAB_COMPAT_H */
