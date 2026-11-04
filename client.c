// rat-lab client: REPL mono-hilo para hablar con el servidor rat-lab.
// Los mensajes asincronos (chat, avisos de conexion) se consultan con `recv`.
// Compatible Linux y Windows (MinGW-w64).
#include "protocol.h"  /* incluye compat.h */

#include <errno.h>

static int g_fd;

// Recibe un mensaje del servidor (bloqueante) y lo imprime.
// Devuelve 0 ok, -1 si la conexion se cerro.
static int wait_reply(void) {
    char type[32];
    char *payload = rl_recv_text(g_fd, type, NULL);
    if (!payload) return -1;
    if (!strcmp(type, "ERR")) printf("error: %s\n", payload);
    else if (!strcmp(type, "PONG")) printf("pong: %s\n", payload);
    else printf("%s", payload); // DATA, OK, INFO, CHAT...
    free(payload);
    return 0;
}

// Drena mensajes asincronos pendientes sin bloquear (comando `recv`).
// Buffer dinámico: crece si llegan muchos mensajes acumulados.
static void drain_pending(void) {
    // Buffer persistente entre llamadas (heap, crece si hace falta).
    static char  *pending = NULL;
    static size_t pcap    = 0;
    static size_t plen    = 0;

    if (!pending) {
        pcap    = 65536;
        pending = malloc(pcap);
        if (!pending) return;
    }

    char buf[16384];
    int  got = 0;
    for (;;) {
        /* select con timeout=0 para no bloquear: compatible Linux y Windows */
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET((unsigned)g_fd, &rset);
        struct timeval tv = {0, 0};
        if (select(g_fd + 1, &rset, NULL, NULL, &tv) <= 0) break;
        ssize_t n = recv(g_fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        // Crecer si no cabe.
        if (plen + (size_t)n > pcap) {
            size_t newcap = pcap * 2 + (size_t)n;
            char  *tmp    = realloc(pending, newcap);
            if (!tmp) break; // descarta lo nuevo si no hay memoria
            pending = tmp;
            pcap    = newcap;
        }
        memcpy(pending + plen, buf, (size_t)n);
        plen += (size_t)n;
        got = 1;
    }
    if (!got && plen == 0) { printf("sin mensajes pendientes\n"); return; }

    // Parsea mensajes completos "TIPO size\n" + payload
    size_t pos = 0;
    while (pos < plen) {
        char *nl = memchr(pending + pos, '\n', plen - pos);
        if (!nl) break;
        char type[32]; unsigned size;
        if (sscanf(pending + pos, "%31s %u", type, &size) != 2) { plen = 0; return; }
        size_t hdr = (size_t)(nl - (pending + pos)) + 1;
        if (plen - pos < hdr + size) break; // mensaje incompleto
        char *payload = pending + pos + hdr;
        printf("[%s] %.*s\n", type, (int)size, payload);
        pos += hdr + size;
    }
    memmove(pending, pending + pos, plen - pos);
    plen -= pos;
}

// Envía el archivo en trozos de CHUNK_SIZE para no cargar todo en RAM.
#define UPLOAD_CHUNK 65536

static int cmd_upload(const char *local, const char *remote) {
    FILE *f = fopen(local, "rb");
    if (!f) { perror("fopen"); return -1; }

    // Tamaño solo para informar al usuario; no es crítico para la transferencia.
    if (fseeko(f, 0, SEEK_END) == 0) {
        off_t sz = ftello(f);
        if (sz >= 0) printf("enviando %.1f KB...\n", sz / 1024.0);
        fseeko(f, 0, SEEK_SET);
    }

    rl_send(g_fd, "UPLOAD", remote, (uint32_t)strlen(remote));
    if (wait_reply() != 0) { fclose(f); return -1; } // "listo para recibir"

    char *chunk = malloc(UPLOAD_CHUNK);
    if (!chunk) { fclose(f); return -1; }

    size_t n;
    while ((n = fread(chunk, 1, UPLOAD_CHUNK, f)) > 0) {
        if (rl_send(g_fd, "FILECHUNK", chunk, (uint32_t)n) != 0) {
            free(chunk); fclose(f); return -1;
        }
    }
    free(chunk);
    fclose(f);

    // Señal de fin de archivo.
    rl_send(g_fd, "FILEEND", NULL, 0);
    return wait_reply(); // "subido"
}

static int cmd_download(const char *remote, const char *local) {
    rl_send(g_fd, "DOWNLOAD", remote, (uint32_t)strlen(remote));
    // El servidor responde en crudo: "FILEDATA <size>\n" + bytes.
    char hdr[64];
    size_t used = 0;
    while (used < sizeof hdr - 1) {
        char c;
        if (read(g_fd, &c, 1) <= 0) return -1;
        if (c == '\n') break;
        hdr[used++] = c;
    }
    hdr[used] = 0;
    long long fsize;
    if (sscanf(hdr, "FILEDATA %lld", &fsize) != 1 || fsize < 0) {
        printf("error: respuesta inesperada\n");
        return -1;
    }
    FILE *f = fopen(local, "wb");
    if (!f) { perror("fopen"); return -1; }
    char buf[8192];
    long long got = 0;
    while (got < fsize) {
        long long want = fsize - got < (long long)sizeof buf ? fsize - got : (long long)sizeof buf;
        ssize_t n = rl_sock_read(g_fd, buf, (size_t)want);
        if (n <= 0) { fclose(f); return -1; }
        fwrite(buf, 1, (size_t)n, f);
        got += n;
    }
    fclose(f);
    printf("guardado: %s (%lld bytes)\n", local, fsize);
    return 0;
}

static void usage(void) {
    printf(
        "comandos:\n"
        "  ping                 latido, el servidor responde PONG\n"
        "  shell <cmd>          ejecuta <cmd> en el servidor\n"
        "  get <remoto> <local> descarga un archivo del servidor\n"
        "  put <local> <remoto> sube un archivo al servidor\n"
        "  chat <mensaje>       difunde un mensaje a los demas clientes\n"
        "  recv                 muestra mensajes/chat/aviso pendientes\n"
        "  sysinfo              info del sistema del servidor\n"
        "  ps                   lista procesos del servidor\n"
        "  kill <pid>           mata un proceso (SIGKILL)\n"
        "  quit                 salir\n");
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : RL_PORT;

    if (rl_net_init() != 0) { fprintf(stderr, "Error al inicializar red\n"); return 1; }
    g_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { fprintf(stderr, "IP invalida\n"); return 1; }
    if (connect(g_fd, (struct sockaddr *)&addr, sizeof addr) != 0) { perror("connect"); return 1; }
    printf("conectado a %s:%d\n", host, port);
    usage();

    char line[1024];
    printf("> ");
    fflush(stdout);
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "quit", 4)) break;
        if (!strncmp(line, "ping", 4)) {
            rl_send(g_fd, "PING", NULL, 0);
            if (wait_reply() != 0) break;
        } else if (!strncmp(line, "shell ", 6)) {
            rl_send(g_fd, "SHELL", line + 6, (uint32_t)strlen(line + 6));
            if (wait_reply() != 0) break;
        } else if (!strncmp(line, "get ", 4)) {
            char r[512], l[512];
            if (sscanf(line + 4, "%511s %511s", r, l) == 2) cmd_download(r, l);
            else printf("uso: get <remoto> <local>\n");
        } else if (!strncmp(line, "put ", 4)) {
            char l[512], r[512];
            if (sscanf(line + 4, "%511s %511s", l, r) == 2) cmd_upload(l, r);
            else printf("uso: put <local> <remoto>\n");
        } else if (!strncmp(line, "chat ", 5)) {
            rl_send(g_fd, "CHAT", line + 5, (uint32_t)strlen(line + 5));
        } else if (!strncmp(line, "recv", 4)) {
            drain_pending();
        } else if (!strncmp(line, "sysinfo", 7)) {
            rl_send(g_fd, "SYSINFO", NULL, 0);
            if (wait_reply() != 0) break;
        } else if (!strncmp(line, "ps", 2)) {
            rl_send(g_fd, "PS", NULL, 0);
            if (wait_reply() != 0) break;
        } else if (!strncmp(line, "kill ", 5)) {
            rl_send(g_fd, "KILL", line + 5, (uint32_t)strlen(line + 5));
            if (wait_reply() != 0) break;
        } else if (line[0]) {
            usage();
        }
        printf("> ");
        fflush(stdout);
    }
    RL_CLOSE_SOCKET(g_fd);
    rl_net_cleanup();
    return 0;
}
