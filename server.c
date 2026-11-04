// rat-lab server: escucha en la LAN, ejecuta comandos, gestiona archivos,
// chat, info del sistema, procesos y notifica conexiones a los clientes.
// Compatible Linux y Windows (MinGW-w64).
#include "protocol.h"  /* incluye compat.h */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#  include <fcntl.h>
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <sys/utsname.h>
#  include <sys/wait.h>
#endif

#define RL_VERSION "rat-lab 0.1"

static pthread_mutex_t g_clients_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_clients[64];
static int g_nclients = 0;
static FILE *g_log;

static void logline(const char *fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    char ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(stdout, "[%s] %s\n", ts, msg);
    fflush(stdout);
    if (g_log) { fprintf(g_log, "[%s] %s\n", ts, msg); fflush(g_log); }
}

// Envía a todos los clientes excepto `except_fd`.
static void broadcast(const char *type, const void *payload, uint32_t size, int except_fd) {
    pthread_mutex_lock(&g_clients_mu);
    for (int i = 0; i < g_nclients; i++)
        if (g_clients[i] != except_fd)
            rl_send(g_clients[i], type, payload, size);
    pthread_mutex_unlock(&g_clients_mu);
}

static void add_client(int fd) {
    pthread_mutex_lock(&g_clients_mu);
    if (g_nclients < 64) g_clients[g_nclients++] = fd;
    pthread_mutex_unlock(&g_clients_mu);
}

static void remove_client(int fd) {
    pthread_mutex_lock(&g_clients_mu);
    for (int i = 0; i < g_nclients; i++)
        if (g_clients[i] == fd) { g_clients[i] = g_clients[--g_nclients]; break; }
    pthread_mutex_unlock(&g_clients_mu);
}

static void send_text(int fd, const char *type, const char *text) {
    rl_send(fd, type, text, (uint32_t)strlen(text));
}

// Ejecuta un comando con popen y devuelve su salida (malloc).
static char *run_capture(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (!out) { pclose(p); return NULL; }
    size_t n;
    while ((n = fread(out + len, 1, cap - len - 1, p)) > 0) {
        len += n;
        if (cap - len - 1 < 1024) { cap *= 2; out = realloc(out, cap); if (!out) break; }
    }
    out[len] = 0;
    pclose(p);
    return out;
}

static void handle_sysinfo(int fd) {
    char buf[4096];
    int off = 0;

#ifdef _WIN32
    /* Nombre de equipo */
    char hostname[256] = "?";
    DWORD hsz = sizeof hostname;
    GetComputerNameA(hostname, &hsz);
    off += snprintf(buf + off, sizeof buf - off, "maquina: %s\n", hostname);

    /* Versión de Windows */
    OSVERSIONINFOA vi;
    vi.dwOSVersionInfoSize = sizeof vi;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (GetVersionExA(&vi))
        off += snprintf(buf + off, sizeof buf - off,
            "sistema: Windows %lu.%lu build %lu\n",
            vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
#pragma GCC diagnostic pop

    /* Uptime */
    ULONGLONG ms = GetTickCount64();
    off += snprintf(buf + off, sizeof buf - off, "uptime: %llu s\n", ms / 1000ULL);

    /* Memoria */
    MEMORYSTATUSEX msx; msx.dwLength = sizeof msx;
    if (GlobalMemoryStatusEx(&msx))
        off += snprintf(buf + off, sizeof buf - off,
            "MemTotal: %llu kB\nMemDisponible: %llu kB\n",
            (unsigned long long)msx.ullTotalPhys / 1024ULL,
            (unsigned long long)msx.ullAvailPhys / 1024ULL);

#else  /* Linux */
    struct utsname u;
    if (uname(&u) == 0)
        off += snprintf(buf + off, sizeof buf - off,
            "sistema: %s %s %s %s\nmaquina: %s\n",
            u.sysname, u.release, u.version, u.machine, u.nodename);
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        double up;
        if (fscanf(f, "%lf", &up) == 1)
            off += snprintf(buf + off, sizeof buf - off, "uptime: %.0f s\n", up);
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        for (int i = 0; i < 3 && fgets(line, sizeof line, f); i++)
            off += snprintf(buf + off, sizeof buf - off, "%s", line);
        fclose(f);
    }
#endif

    send_text(fd, "DATA", buf);
}

static void handle_ps(int fd) {
    size_t cap = 65536, len = 0;
    char *out = malloc(cap);
    if (!out) return;

#ifdef _WIN32
    len += snprintf(out, cap, "%-8s %-30s\n", "PID", "NOMBRE");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        free(out); send_text(fd, "ERR", "snapshot fallido"); return;
    }
    PROCESSENTRY32 pe; pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do {
            int n = snprintf(out + len, cap - len,
                "%-8lu %-30s\n", (unsigned long)pe.th32ProcessID, pe.szExeFile);
            if (n < 0) break;
            len += (size_t)n;
            if (cap - len < 1024) {
                cap *= 2; out = realloc(out, cap);
                if (!out) { CloseHandle(snap); return; }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);

#else  /* Linux */
    len += snprintf(out, cap, "%-8s %-20s %s\n", "PID", "NOMBRE", "ESTADO");
    DIR *d = opendir("/proc");
    if (!d) { free(out); send_text(fd, "ERR", "no /proc"); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[300], comm[256] = "?", state = '?';
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
            fclose(f);
            comm[strcspn(comm, "\n")] = 0;
        }
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        f = fopen(path, "r");
        if (f) {
            int c;
            while ((c = fgetc(f)) != ')' && c != EOF);
            if (fgetc(f) == ' ') state = (char)fgetc(f);
            fclose(f);
        }
        int n = snprintf(out + len, cap - len, "%-8s %-20s %c\n", e->d_name, comm, state);
        if (n < 0) break;
        len += (size_t)n;
        if (cap - len < 1024) {
            cap *= 2; out = realloc(out, cap);
            if (!out) { closedir(d); return; }
        }
    }
    closedir(d);
#endif

    rl_send(fd, "DATA", out, (uint32_t)len);
    free(out);
}

static void handle_download(int fd, const char *rpath) {
    FILE *f = fopen(rpath, "rb");
    if (!f) { send_text(fd, "ERR", strerror(errno)); return; }
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); send_text(fd, "ERR", "fseeko"); return; }
    off_t size = ftello(f);
    if (size < 0) { fclose(f); return; }
    fseeko(f, 0, SEEK_SET);
    // respuesta en crudo: "FILEDATA <size>\n" + bytes
    char hdr[64];
    int hl = snprintf(hdr, sizeof hdr, "FILEDATA %lld\n", (long long)size);
    if (write(fd, hdr, (size_t)hl) != hl) { fclose(f); return; }
    char buf[8192]; ssize_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        char *p = buf; ssize_t sent = 0;
        while (sent < n) { ssize_t w = write(fd, p + sent, (size_t)(n - sent)); if (w <= 0) { fclose(f); return; } sent += w; }
    }
    fclose(f);
}

// Contexto por cliente: fd + ruta de upload pendiente (evita la variable global compartida).
typedef struct {
    int fd;
    char pending_upload[300];
} client_ctx_t;

static void *client_thread(void *arg) {
    client_ctx_t *ctx = arg;
    int fd = ctx->fd;
    char type[32];
    for (;;) {
        long size = rl_recv_header(fd, type);
        if (size < 0) break;
        char *payload = malloc((size_t)size + 1);
        if (!payload) break;
        if (rl_read_full(fd, payload, (uint32_t)size) != 0) { free(payload); break; }
        payload[size] = 0;

        if (!strcmp(type, "PING")) {
            send_text(fd, "PONG", RL_VERSION);
        } else if (!strcmp(type, "SHELL")) {
            logline("shell: %s", payload);
            char *out = run_capture(payload);
            send_text(fd, "DATA", out ? out : "error");
            free(out);
        } else if (!strcmp(type, "DOWNLOAD")) {
            logline("download: %s", payload);
            handle_download(fd, payload);
        } else if (!strcmp(type, "UPLOAD")) {
            logline("upload a: %s", payload);
            snprintf(ctx->pending_upload, sizeof ctx->pending_upload, "%s", payload);
            send_text(fd, "OK", "listo para recibir");
        } else if (!strcmp(type, "FILECHUNK")) {
            // Recibe un trozo del archivo y lo añade.
            if (ctx->pending_upload[0] == '\0') {
                send_text(fd, "ERR", "sin upload activo");
            } else {
                FILE *f = fopen(ctx->pending_upload, "ab");
                if (!f) send_text(fd, "ERR", strerror(errno));
                else { fwrite(payload, 1, (size_t)size, f); fclose(f); }
            }
        } else if (!strcmp(type, "FILEEND")) {
            send_text(fd, "OK", "subido");
            ctx->pending_upload[0] = '\0';
        } else if (!strcmp(type, "FILE")) {
            // Compatibilidad con clientes anteriores (archivo completo en un mensaje).
            FILE *f = fopen(ctx->pending_upload, "wb");
            if (!f) send_text(fd, "ERR", strerror(errno));
            else { fwrite(payload, 1, (uint32_t)size, f); fclose(f); send_text(fd, "OK", "subido"); }
            ctx->pending_upload[0] = '\0';
        } else if (!strcmp(type, "CHAT")) {
            logline("chat: %s", payload);
            broadcast("CHAT", payload, (uint32_t)size, fd);
        } else if (!strcmp(type, "SYSINFO")) {
            handle_sysinfo(fd);
        } else if (!strcmp(type, "PS")) {
            handle_ps(fd);
        } else if (!strcmp(type, "KILL")) {
#ifdef _WIN32
            DWORD wpid = (DWORD)atol(payload);
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, wpid);
            if (h && TerminateProcess(h, 1)) { CloseHandle(h); send_text(fd, "OK", "senal enviada"); }
            else send_text(fd, "ERR", "no se pudo terminar el proceso");
#else
            pid_t pid = (pid_t)atoi(payload);
            if (kill(pid, SIGKILL) == 0) send_text(fd, "OK", "senal enviada");
            else send_text(fd, "ERR", strerror(errno));
#endif
        } else {
            send_text(fd, "ERR", "comando desconocido");
        }
        free(payload);
    }
    remove_client(fd);
    RL_CLOSE_SOCKET(fd);
    free(ctx);
    logline("cliente desconectado");
    broadcast("INFO", "un cliente se desconecto", 25, -1);
    return NULL;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : RL_PORT;
    g_log = fopen("ratlab-server.log", "a");

    if (rl_net_init() != 0) { fprintf(stderr, "Error al inicializar red\n"); return 1; }
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    int srv = (int)socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0) { perror("bind"); return 1; }
    if (listen(srv, 16) != 0) { perror("listen"); return 1; }

    char hostname[256] = "?";
    gethostname(hostname, sizeof hostname);
    logline("%s escuchando en 0.0.0.0:%d (maquina %s)", RL_VERSION, port, hostname);

    for (;;) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof caddr;
        int cfd = (int)accept(srv, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof ip);
        logline("conexion entrante de %s:%d", ip, ntohs(caddr.sin_port));
        char note[128];
        int n = snprintf(note, sizeof note, "nuevo cliente conectado desde %s", ip);
        broadcast("INFO", note, (uint32_t)n, cfd);
        add_client(cfd);
        client_ctx_t *ctx = calloc(1, sizeof(client_ctx_t));
        if (!ctx) { close(cfd); remove_client(cfd); continue; }
        ctx->fd = cfd;
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, ctx) != 0) { RL_CLOSE_SOCKET(cfd); remove_client(cfd); free(ctx); continue; }
        pthread_detach(th);
    }
    return 0;
}
