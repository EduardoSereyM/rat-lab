// rat-lab: protocolo de mensajes compartido entre server y client.
// Formato de cada mensaje:
//   "TIPO size\n"  (size en bytes del payload, puede ser 0)
//   seguido de exactamente `size` bytes de payload (texto o binario).
#ifndef RATLAB_PROTOCOL_H
#define RATLAB_PROTOCOL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"  /* rl_sock_write, rl_sock_read, RL_CLOSE_SOCKET */

#define RL_PORT 44445
#define RL_MAX_PAYLOAD (64 * 1024 * 1024) // 64 MiB

// Envía un mensaje completo. Devuelve 0 ok, -1 error.
static inline int rl_send(int fd, const char *type, const void *payload, uint32_t size) {
    char hdr[64];
    int hdr_len = snprintf(hdr, sizeof hdr, "%s %u\n", type, size);
    if (hdr_len <= 0) return -1;
    if (rl_sock_write(fd, hdr, (size_t)hdr_len) != hdr_len) return -1;
    const char *p = (const char *)payload;
    uint32_t sent = 0;
    while (sent < size) {
        ssize_t n = rl_sock_write(fd, p + sent, size - sent);
        if (n <= 0) return -1;
        sent += (uint32_t)n;
    }
    return 0;
}

// Recibe la cabecera de un mensaje: escribe el tipo en `type` (cap 32) y
// devuelve el tamaño del payload, o -1 en error.
static inline long rl_recv_header(int fd, char *type) {
    char hdr[96];
    size_t used = 0;
    while (used < sizeof hdr - 1) {
        char c;
        ssize_t n = rl_sock_read(fd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n') break;
        hdr[used++] = c;
    }
    hdr[used] = 0;
    unsigned size;
    if (sscanf(hdr, "%31s %u", type, &size) != 2) return -1;
    if (size > RL_MAX_PAYLOAD) return -1;
    return (long)size;
}

// Lee exactamente `size` bytes. Devuelve 0 ok, -1 error.
static inline int rl_read_full(int fd, void *buf, uint32_t size) {
    char *p = (char *)buf;
    uint32_t got = 0;
    while (got < size) {
        ssize_t n = rl_sock_read(fd, p + got, size - got);
        if (n <= 0) return -1;
        got += (uint32_t)n;
    }
    return 0;
}

// Recibe un mensaje de texto completo en un buffer malloc'eado (con NUL).
// Devuelve puntero o NULL. `*out_size` opcional.
static inline char *rl_recv_text(int fd, char *type, uint32_t *out_size) {
    long size = rl_recv_header(fd, type);
    if (size < 0) return NULL;
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) return NULL;
    if (rl_read_full(fd, buf, (uint32_t)size) != 0) { free(buf); return NULL; }
    buf[size] = 0;
    if (out_size) *out_size = (uint32_t)size;
    return buf;
}

#endif
