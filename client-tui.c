// client-tui.c — cliente rat-lab con interfaz TUI (ncurses).
// Solo Linux (el cliente REPL funciona en Windows igualmente).
//
// Layout:
//   Línea 0        : barra de título (host:puerto)
//   Líneas 1..H-3  : área de salida con scroll automático
//   Línea H-2      : barra de ayuda / atajos
//   Línea H-1      : línea de entrada
//
// Atajos:
//   F1  → ayuda         F2 → sysinfo
//   F3  → lista procesos F5 → ping
//   Ctrl-U → borrar línea de entrada
//   quit / exit → desconectar
#include "protocol.h"

#include <locale.h>
#include <ncurses.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#define UPLOAD_CHUNK 65536

// ── Ventanas ──────────────────────────────────────────────────────────
static WINDOW *w_title;
static WINDOW *w_out;
static WINDOW *w_hints;
static WINDOW *w_inp;

// ── Pares de color ────────────────────────────────────────────────────
#define CP_DEFAULT 1  // texto normal (DATA, shell)
#define CP_CHAT    2  // cian   → mensajes de chat
#define CP_ERR     3  // rojo   → errores
#define CP_INFO    4  // amarillo → avisos INFO
#define CP_OK      5  // verde  → OK / PONG
#define CP_TS      6  // apagado → timestamp
#define CP_TITLE   7  // blanco/azul → barra de título
#define CP_HINTS   8  // negro/blanco → barra de ayuda
#define CP_INPUT   9  // blanco brillante → prompt

// ── Estado global ────────────────────────────────────────────────────
static int             g_fd      = -1;
static volatile int    g_running = 1;
static pthread_mutex_t nc_mu     = PTHREAD_MUTEX_INITIALIZER;

// Línea de entrada
static char g_input[1024];
static int  g_inp_len = 0;

// Transferencia de archivo: pausa el hilo receptor mientras
// el hilo principal usa el socket de forma síncrona.
static volatile int g_ft_active = 0;
static char         g_dl_dest[512] = "";

// ── Timestamp ─────────────────────────────────────────────────────────
static void get_ts(char *buf, size_t n) {
    time_t t = time(NULL);
    strftime(buf, n, "%H:%M:%S", localtime(&t));
}

// ── Redibujar prompt de entrada (llamar con nc_mu tomado) ─────────────
static void redraw_input(void) {
    int cols = getmaxx(w_inp);
    werase(w_inp);
    wattron(w_inp, COLOR_PAIR(CP_INPUT) | A_BOLD);
    mvwaddstr(w_inp, 0, 0, "> ");
    wattroff(w_inp, A_BOLD);
    // Desplazar si la línea no cabe
    int start = g_inp_len > cols - 3 ? g_inp_len - (cols - 3) : 0;
    waddnstr(w_inp, g_input + start, g_inp_len - start);
    wattroff(w_inp, COLOR_PAIR(CP_INPUT));
    wrefresh(w_inp);
}

// ── Añadir línea al área de salida (thread-safe) ──────────────────────
static void out_add(int cp, const char *fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    char ts[16];
    get_ts(ts, sizeof ts);

    pthread_mutex_lock(&nc_mu);
    wattron(w_out, COLOR_PAIR(CP_TS));
    wprintw(w_out, "[%s] ", ts);
    wattroff(w_out, COLOR_PAIR(CP_TS));
    wattron(w_out, COLOR_PAIR(cp));
    wprintw(w_out, "%s\n", msg);
    wattroff(w_out, COLOR_PAIR(cp));
    wrefresh(w_out);
    redraw_input();      // restaurar cursor de entrada
    pthread_mutex_unlock(&nc_mu);
}

// ── Hilo receptor ─────────────────────────────────────────────────────
static void *recv_thread(void *arg) {
    (void)arg;
    while (g_running) {
        // Pausa cuando el hilo principal hace una transferencia síncrona
        if (g_ft_active) {
            usleep(10000);
            continue;
        }

        // select con timeout de 50 ms → revisa g_running y g_ft_active con frecuencia
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET((unsigned)g_fd, &rset);
        struct timeval tv = {0, 50000};
        if (select(g_fd + 1, &rset, NULL, NULL, &tv) <= 0) continue;

        // Leer cabecera del protocolo
        char type[32];
        long size = rl_recv_header(g_fd, type);
        if (size < 0) {
            g_running = 0;
            break;
        }

        // Descarga de archivo en streaming (evita cargar todo en RAM)
        if (!strcmp(type, "FILEDATA") && g_dl_dest[0]) {
            char dest[512];
            strncpy(dest, g_dl_dest, sizeof dest - 1);
            dest[sizeof dest - 1] = '\0';

            FILE *f = fopen(dest, "wb");
            char buf[8192];
            long got = 0;
            while (got < size) {
                long want = size - got < (long)sizeof buf
                            ? size - got : (long)sizeof buf;
                ssize_t n = rl_sock_read(g_fd, buf, (size_t)want);
                if (n <= 0) break;
                if (f) fwrite(buf, 1, (size_t)n, f);
                got += n;
            }
            if (f) fclose(f);
            g_dl_dest[0] = '\0';
            out_add(CP_OK, "guardado: %s  (%ld bytes)", dest, size);
            continue;
        }

        // Mensaje normal: leer payload
        char *payload = malloc((size_t)size + 1);
        if (!payload) { g_running = 0; break; }
        if (size > 0 && rl_read_full(g_fd, payload, (uint32_t)size) != 0) {
            free(payload);
            g_running = 0;
            break;
        }
        payload[size] = '\0';

        // Elegir color según tipo
        int cp = CP_DEFAULT;
        if      (!strcmp(type, "CHAT"))                     cp = CP_CHAT;
        else if (!strcmp(type, "ERR"))                      cp = CP_ERR;
        else if (!strcmp(type, "INFO"))                     cp = CP_INFO;
        else if (!strcmp(type, "OK") || !strcmp(type, "PONG")) cp = CP_OK;

        // Separar líneas largas (p.ej. salida de shell multilínea)
        char *line = strtok(payload, "\n");
        while (line) {
            out_add(cp, "[%s] %s", type, line);
            line = strtok(NULL, "\n");
        }
        free(payload);
    }

    out_add(CP_ERR, "conexion cerrada");
    return NULL;
}

// ── Enviar mensaje simple ─────────────────────────────────────────────
static void send_cmd(const char *type, const char *payload) {
    rl_send(g_fd, type, payload,
            payload ? (uint32_t)strlen(payload) : 0);
}

// ── Upload en chunks (hilo principal, sincrónico) ─────────────────────
static void do_upload(const char *local, const char *remote) {
    FILE *f = fopen(local, "rb");
    if (!f) { out_add(CP_ERR, "no se puede abrir: %s", local); return; }

    // Tamaño solo para informar
    if (fseeko(f, 0, SEEK_END) == 0) {
        off_t sz = ftello(f);
        if (sz >= 0)
            out_add(CP_INFO, "enviando %.1f KB → %s ...", sz / 1024.0, remote);
        fseeko(f, 0, SEEK_SET);
    }

    // Pausar receptor y tomar el socket
    g_ft_active = 1;
    usleep(100000); // 100 ms > timeout select del receptor (50 ms)

    send_cmd("UPLOAD", remote);

    // Leer "listo para recibir" directo
    char type[32];
    char *resp = rl_recv_text(g_fd, type, NULL);
    if (resp) { out_add(strcmp(type, "ERR") ? CP_INFO : CP_ERR,
                        "[%s] %s", type, resp); free(resp); }

    // Enviar en chunks
    char *chunk = malloc(UPLOAD_CHUNK);
    if (chunk) {
        size_t n;
        while ((n = fread(chunk, 1, UPLOAD_CHUNK, f)) > 0)
            rl_send(g_fd, "FILECHUNK", chunk, (uint32_t)n);
        free(chunk);
    }
    fclose(f);

    rl_send(g_fd, "FILEEND", NULL, 0);
    resp = rl_recv_text(g_fd, type, NULL);
    if (resp) { out_add(strcmp(type, "ERR") ? CP_OK : CP_ERR,
                        "[%s] %s", type, resp); free(resp); }

    g_ft_active = 0;
}

// ── Procesar comando introducido por el usuario ───────────────────────
static void process_command(const char *line) {
    if (!line[0]) return;

    if (!strcmp(line, "quit") || !strcmp(line, "exit")) {
        g_running = 0;

    } else if (!strcmp(line, "ping")) {
        send_cmd("PING", NULL);

    } else if (!strcmp(line, "sysinfo")) {
        send_cmd("SYSINFO", NULL);

    } else if (!strcmp(line, "ps")) {
        send_cmd("PS", NULL);

    } else if (!strncmp(line, "shell ", 6)) {
        send_cmd("SHELL", line + 6);

    } else if (!strncmp(line, "chat ", 5)) {
        out_add(CP_CHAT, "[yo] %s", line + 5);
        send_cmd("CHAT", line + 5);

    } else if (!strncmp(line, "kill ", 5)) {
        send_cmd("KILL", line + 5);

    } else if (!strncmp(line, "get ", 4)) {
        char r[512], l[512];
        if (sscanf(line + 4, "%511s %511s", r, l) == 2) {
            strncpy(g_dl_dest, l, sizeof g_dl_dest - 1);
            g_dl_dest[sizeof g_dl_dest - 1] = '\0';
            send_cmd("DOWNLOAD", r);
            out_add(CP_INFO, "descargando %s → %s ...", r, l);
        } else {
            out_add(CP_ERR, "uso: get <remoto> <local>");
        }

    } else if (!strncmp(line, "put ", 4)) {
        char lo[512], re[512];
        if (sscanf(line + 4, "%511s %511s", lo, re) == 2)
            do_upload(lo, re);
        else
            out_add(CP_ERR, "uso: put <local> <remoto>");

    } else if (!strcmp(line, "help") || !strcmp(line, "?")) {
        out_add(CP_INFO, "--- comandos -------------------------------------------");
        out_add(CP_INFO, "  ping                    latido");
        out_add(CP_INFO, "  sysinfo                 info del sistema remoto");
        out_add(CP_INFO, "  ps                      lista procesos remotos");
        out_add(CP_INFO, "  shell <cmd>             ejecutar comando remoto");
        out_add(CP_INFO, "  chat <mensaje>          enviar chat a todos");
        out_add(CP_INFO, "  get <remoto> <local>    descargar archivo");
        out_add(CP_INFO, "  put <local> <remoto>    subir archivo");
        out_add(CP_INFO, "  kill <pid>              terminar proceso");
        out_add(CP_INFO, "  quit                    salir");
        out_add(CP_INFO, "--- atajos ---------------------------------------------");
        out_add(CP_INFO, "  F1=ayuda  F2=sysinfo  F3=ps  F5=ping  Ctrl-U=borrar");

    } else {
        out_add(CP_ERR, "desconocido: '%s'  (escribe 'help')", line);
    }
}

// ── Inicializar ncurses y crear ventanas ──────────────────────────────
static void init_ui(const char *host, int port) {
    setlocale(LC_ALL, "");  // habilita UTF-8 en ncurses
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    halfdelay(2);   // wgetch espera máx 200 ms (permite revisar g_running)
    curs_set(1);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_DEFAULT, -1,            -1);
        init_pair(CP_CHAT,    COLOR_CYAN,    -1);
        init_pair(CP_ERR,     COLOR_RED,     -1);
        init_pair(CP_INFO,    COLOR_YELLOW,  -1);
        init_pair(CP_OK,      COLOR_GREEN,   -1);
        init_pair(CP_TS,      COLOR_WHITE,   -1);
        init_pair(CP_TITLE,   COLOR_WHITE,   COLOR_BLUE);
        init_pair(CP_HINTS,   COLOR_BLACK,   COLOR_WHITE);
        init_pair(CP_INPUT,   COLOR_WHITE,   -1);
    }

    int rows = LINES, cols = COLS;

    // ── Barra de título ──────────────────────────────────────────────
    w_title = newwin(1, cols, 0, 0);
    wbkgd(w_title, COLOR_PAIR(CP_TITLE));
    wattron(w_title, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(w_title, 0, 1, " rat-lab  •  %s:%d ", host, port);
    wattroff(w_title, COLOR_PAIR(CP_TITLE) | A_BOLD);
    wrefresh(w_title);

    // ── Área de salida (desplazamiento automático) ───────────────────
    w_out = newwin(rows - 3, cols, 1, 0);
    scrollok(w_out, TRUE);
    idlok(w_out, TRUE);
    wrefresh(w_out);

    // ── Barra de ayuda ───────────────────────────────────────────────
    w_hints = newwin(1, cols, rows - 2, 0);
    wbkgd(w_hints, COLOR_PAIR(CP_HINTS));
    wattron(w_hints, COLOR_PAIR(CP_HINTS));
    mvwprintw(w_hints, 0, 1,
        " F1:ayuda  F2:sysinfo  F3:ps  F5:ping  Ctrl-U:borrar  quit:salir ");
    wattroff(w_hints, COLOR_PAIR(CP_HINTS));
    wrefresh(w_hints);

    // ── Línea de entrada ─────────────────────────────────────────────
    w_inp = newwin(1, cols, rows - 1, 0);
    keypad(w_inp, TRUE);  // necesario para que wgetch(w_inp) traduzca F-keys
    wrefresh(w_inp);
}

// ── main ──────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port         = argc > 2 ? atoi(argv[2]) : RL_PORT;

    if (rl_net_init() != 0) {
        fprintf(stderr, "Error al inicializar la red\n");
        return 1;
    }

    g_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "IP inválida: %s\n", host);
        return 1;
    }
    if (connect(g_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("connect");
        return 1;
    }

    init_ui(host, port);

    // Primer mensaje de bienvenida
    out_add(CP_OK,   "conectado a %s:%d", host, port);
    out_add(CP_INFO, "escribe 'help' o pulsa F1 para ver los comandos disponibles");

    // Arrancar hilo receptor
    pthread_t th;
    pthread_create(&th, NULL, recv_thread, NULL);

    // Dibujar prompt inicial
    pthread_mutex_lock(&nc_mu);
    redraw_input();
    pthread_mutex_unlock(&nc_mu);

    // ── Bucle principal de entrada ────────────────────────────────────
    while (g_running) {
        int c = wgetch(w_inp);  // bloqueante con timeout de 200 ms (halfdelay)
        if (c == ERR) continue; // timeout → revisar g_running y volver

        if (c == '\n' || c == '\r' || c == KEY_ENTER) {
            // Enviar comando
            char cmd[1024];
            pthread_mutex_lock(&nc_mu);
            strncpy(cmd, g_input, sizeof cmd - 1);
            cmd[sizeof cmd - 1] = '\0';
            g_input[0] = '\0';
            g_inp_len  = 0;
            redraw_input();
            pthread_mutex_unlock(&nc_mu);
            process_command(cmd);

        } else if (c == KEY_BACKSPACE || c == 127 || c == '\b') {
            pthread_mutex_lock(&nc_mu);
            if (g_inp_len > 0) g_input[--g_inp_len] = '\0';
            redraw_input();
            pthread_mutex_unlock(&nc_mu);

        } else if (c == ('u' & 0x1f)) {  // Ctrl-U → limpiar línea
            pthread_mutex_lock(&nc_mu);
            g_input[0] = '\0';
            g_inp_len  = 0;
            redraw_input();
            pthread_mutex_unlock(&nc_mu);

        } else if (c == KEY_F(1)) {
            process_command("help");

        } else if (c == KEY_F(2)) {
            process_command("sysinfo");

        } else if (c == KEY_F(3)) {
            process_command("ps");

        } else if (c == KEY_F(5)) {
            process_command("ping");

        } else if (c >= 32 && c < 127) {
            // Carácter imprimible
            pthread_mutex_lock(&nc_mu);
            if (g_inp_len < (int)sizeof g_input - 1) {
                g_input[g_inp_len++] = (char)c;
                g_input[g_inp_len]   = '\0';
            }
            redraw_input();
            pthread_mutex_unlock(&nc_mu);
        }
    }

    endwin();
    pthread_join(th, NULL);
    RL_CLOSE_SOCKET(g_fd);
    rl_net_cleanup();
    return 0;
}
