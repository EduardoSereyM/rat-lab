// client-gui.cpp — cliente rat-lab con interfaz gráfica (Dear ImGui + GLFW + OpenGL3).
// Compilar: ver Makefile, target ratlab-gui
//
// Pestañas: Chat | Shell | Sistema | Procesos | Archivos
// Hilo receptor separado para mensajes asíncronos del servidor.

#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

extern "C" {
#include "protocol.h"   // incluye compat.h
}

#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

// ── Estructura de mensaje ─────────────────────────────────────────────
struct Msg {
    std::string ts;      // timestamp "HH:MM:SS"
    std::string type;    // CHAT, DATA, ERR, OK, INFO, PONG...
    std::string text;
};

// ── Estado global de la aplicación ───────────────────────────────────
static int              g_fd      = -1;
static std::atomic<bool> g_running{false};
static std::mutex       g_mu;
static std::vector<Msg> g_msgs;      // todos los mensajes recibidos
static std::string      g_sysinfo;   // último sysinfo
static std::string      g_ps_text;   // último ps
static std::string      g_shell_out; // salida acumulada de shell

// Descarga en curso
static std::string      g_dl_dest;

// ── Colores por tipo de mensaje ───────────────────────────────────────
static ImVec4 color_for_type(const std::string &t) {
    if (t == "CHAT")  return {0.4f, 0.9f, 0.9f, 1.f};  // cian
    if (t == "ERR")   return {1.0f, 0.4f, 0.4f, 1.f};  // rojo
    if (t == "INFO")  return {1.0f, 0.9f, 0.3f, 1.f};  // amarillo
    if (t == "OK" || t == "PONG") return {0.4f, 0.9f, 0.4f, 1.f}; // verde
    return {0.85f, 0.85f, 0.85f, 1.f}; // blanco apagado
}

// ── Timestamp ─────────────────────────────────────────────────────────
static std::string now_ts() {
    char buf[16];
    time_t t = time(nullptr);
    strftime(buf, sizeof buf, "%H:%M:%S", localtime(&t));
    return buf;
}

// ── Añadir mensaje thread-safe ────────────────────────────────────────
static void push_msg(const std::string &type, const std::string &text) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_msgs.push_back({now_ts(), type, text});
    // limitar historial a 500 mensajes
    if (g_msgs.size() > 500) g_msgs.erase(g_msgs.begin());
}

// ── Hilo receptor ────────────────────────────────────────────────────
static void recv_thread_fn() {
    while (g_running) {
        // select con timeout 50 ms para revisar g_running
        fd_set rset; FD_ZERO(&rset); FD_SET((unsigned)g_fd, &rset);
        struct timeval tv = {0, 50000};
        if (select(g_fd + 1, &rset, NULL, NULL, &tv) <= 0) continue;

        char type[32];
        long size = rl_recv_header(g_fd, type);
        if (size < 0) { g_running = false; break; }

        // Descarga de archivo en streaming (desbloquear mutex antes del I/O)
        std::string dl_dest_copy;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (std::string(type) == "FILEDATA" && !g_dl_dest.empty()) {
                dl_dest_copy = g_dl_dest;
                g_dl_dest.clear();
            }
        }
        if (!dl_dest_copy.empty()) {
            FILE *f = fopen(dl_dest_copy.c_str(), "wb");
            char buf[8192]; long got = 0;
            while (got < size) {
                long want = size - got < (long)sizeof buf ? size - got : (long)sizeof buf;
                ssize_t n = rl_sock_read(g_fd, buf, (size_t)want);
                if (n <= 0) break;
                if (f) fwrite(buf, 1, (size_t)n, f);
                got += n;
            }
            if (f) fclose(f);
            push_msg("OK", "guardado: " + dl_dest_copy + " (" + std::to_string(size) + " bytes)");
            continue;
        }

        char *payload = (char *)malloc((size_t)size + 1);
        if (!payload) { g_running = false; break; }
        if (size > 0 && rl_read_full(g_fd, payload, (uint32_t)size) != 0) {
            free(payload); g_running = false; break;
        }
        payload[size] = '\0';

        std::string stype(type), spayload(payload);
        free(payload);

        // Distribuir a las vistas específicas además del log general
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (stype == "DATA") {
                // Heurística: si contiene "sistema:" es sysinfo, si tiene columnas PID es ps
                if (spayload.find("sistema:") != std::string::npos ||
                    spayload.find("maquina:") != std::string::npos ||
                    spayload.find("sistema: Windows") != std::string::npos ||
                    spayload.find("uptime:") != std::string::npos) {
                    g_sysinfo = spayload;
                } else if (spayload.find("PID") != std::string::npos &&
                           spayload.find("NOMBRE") != std::string::npos) {
                    g_ps_text = spayload;
                } else {
                    g_shell_out += spayload + "\n";
                }
            }
        }

        // Dividir por líneas para el log de chat/mensajes
        std::string line;
        for (char c : spayload) {
            if (c == '\n') {
                if (!line.empty()) push_msg(stype, line);
                line.clear();
            } else {
                line += c;
            }
        }
        if (!line.empty()) push_msg(stype, line);
    }
    push_msg("ERR", "conexion cerrada");
}

// ── Enviar texto simple ───────────────────────────────────────────────
static void send_cmd(const char *type, const char *payload) {
    if (g_fd < 0 || !g_running) return;
    rl_send(g_fd, type, payload, payload ? (uint32_t)strlen(payload) : 0);
}

// ── Upload en chunks ─────────────────────────────────────────────────
static void do_upload(const char *local, const char *remote) {
    FILE *f = fopen(local, "rb");
    if (!f) { push_msg("ERR", std::string("no se puede abrir: ") + local); return; }
    send_cmd("UPLOAD", remote);
    char *chunk = (char *)malloc(65536);
    if (chunk) {
        size_t n;
        while ((n = fread(chunk, 1, 65536, f)) > 0)
            rl_send(g_fd, "FILECHUNK", chunk, (uint32_t)n);
        free(chunk);
    }
    fclose(f);
    rl_send(g_fd, "FILEEND", nullptr, 0);
    push_msg("INFO", std::string("upload iniciado: ") + local + " -> " + remote);
}

// ── Conectar al servidor ──────────────────────────────────────────────
static bool do_connect(const char *host, int port) {
    if (g_fd >= 0) { RL_CLOSE_SOCKET(g_fd); g_fd = -1; }
    g_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return false;
    if (connect(g_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        RL_CLOSE_SOCKET(g_fd); g_fd = -1; return false;
    }
    g_running = true;
    std::thread(recv_thread_fn).detach();
    push_msg("OK", std::string("conectado a ") + host + ":" + std::to_string(port));
    return true;
}

static void do_disconnect() {
    g_running = false;
    if (g_fd >= 0) { RL_CLOSE_SOCKET(g_fd); g_fd = -1; }
}

// ── Renderizar pestaña Chat ───────────────────────────────────────────
static void tab_chat() {
    static char chat_buf[1024] = "";

    // Área de mensajes con scroll automático
    float input_h = ImGui::GetFrameHeightWithSpacing() + 8;
    ImVec2 log_size(0, -input_h);
    ImGui::BeginChild("chat_log", log_size, true);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto &m : g_msgs) {
            ImGui::TextDisabled("[%s]", m.ts.c_str());
            ImGui::SameLine();
            ImGui::TextColored(color_for_type(m.type), "[%s]", m.type.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", m.text.c_str());
        }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // Entrada de chat
    ImGui::PushItemWidth(-80);
    bool send = ImGui::InputText("##chat", chat_buf, sizeof chat_buf,
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if ((ImGui::Button("Enviar") || send) && chat_buf[0]) {
        push_msg("CHAT", std::string("[yo] ") + chat_buf);
        send_cmd("CHAT", chat_buf);
        chat_buf[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }
}

// ── Renderizar pestaña Shell ──────────────────────────────────────────
static void tab_shell() {
    static char cmd_buf[1024] = "";

    float input_h = ImGui::GetFrameHeightWithSpacing() + 8;
    ImVec2 out_size(0, -input_h);
    ImGui::BeginChild("shell_out", out_size, true);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ImGui::TextUnformatted(g_shell_out.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::PushItemWidth(-130);
    bool run = ImGui::InputText("##shell", cmd_buf, sizeof cmd_buf,
                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if ((ImGui::Button("Ejecutar") || run) && cmd_buf[0]) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_shell_out += std::string("> ") + cmd_buf + "\n";
        }
        send_cmd("SHELL", cmd_buf);
        cmd_buf[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Limpiar")) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_shell_out.clear();
    }
}

// ── Renderizar pestaña Sistema ────────────────────────────────────────
static void tab_sistema() {
    if (ImGui::Button("Actualizar")) send_cmd("SYSINFO", nullptr);
    ImGui::SameLine();
    if (ImGui::Button("Ping")) send_cmd("PING", nullptr);

    ImGui::Separator();
    ImGui::BeginChild("sysinfo_view", ImVec2(0, 0), true);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_sysinfo.empty())
            ImGui::TextDisabled("(presiona Actualizar)");
        else
            ImGui::TextUnformatted(g_sysinfo.c_str());
    }
    ImGui::EndChild();
}

// ── Renderizar pestaña Procesos ───────────────────────────────────────
static void tab_procesos() {
    static char kill_buf[32] = "";

    if (ImGui::Button("Listar procesos")) send_cmd("PS", nullptr);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputText("PID##kill", kill_buf, sizeof kill_buf);
    ImGui::SameLine();
    if (ImGui::Button("Kill") && kill_buf[0]) {
        send_cmd("KILL", kill_buf);
        kill_buf[0] = '\0';
    }

    ImGui::Separator();
    ImGui::BeginChild("ps_view", ImVec2(0, 0), true);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_ps_text.empty())
            ImGui::TextDisabled("(presiona Listar procesos)");
        else
            ImGui::TextUnformatted(g_ps_text.c_str());
    }
    ImGui::EndChild();
}

// ── Renderizar pestaña Archivos ───────────────────────────────────────
static void tab_archivos() {
    static char local_buf[512]  = "";
    static char remote_buf[512] = "";

    ImGui::Text("Ruta local:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300);
    ImGui::InputText("##local", local_buf, sizeof local_buf);

    ImGui::Text("Ruta remota:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300);
    ImGui::InputText("##remote", remote_buf, sizeof remote_buf);

    ImGui::Spacing();
    if (ImGui::Button("Subir (local -> remoto)") && local_buf[0] && remote_buf[0])
        do_upload(local_buf, remote_buf);

    ImGui::SameLine();
    if (ImGui::Button("Descargar (remoto -> local)") && local_buf[0] && remote_buf[0]) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_dl_dest = local_buf;
        }
        send_cmd("DOWNLOAD", remote_buf);
        push_msg("INFO", std::string("descargando ") + remote_buf + " -> " + local_buf);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Ruta remota: ruta absoluta en el servidor");
    ImGui::TextDisabled("Ruta local:  ruta absoluta en esta maquina");
}

// ── Ventana de conexión (pantalla inicial) ────────────────────────────
static void window_connect() {
    static char host_buf[256] = "127.0.0.1";
    static int  port_val      = RL_PORT;
    static char err_msg[300]  = "";

    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos({io.DisplaySize.x * .5f, io.DisplaySize.y * .5f},
                            ImGuiCond_Always, {.5f, .5f});
    ImGui::SetNextWindowSize({340, 180}, ImGuiCond_Always);
    ImGui::Begin("Conectar##dlg", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse);

    ImGui::Text("Host:");  ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##host", host_buf, sizeof host_buf);

    ImGui::Text("Puerto:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##port", &port_val, 0);

    ImGui::Spacing();
    if (ImGui::Button("Conectar", {120, 0})) {
        if (do_connect(host_buf, port_val))
            err_msg[0] = '\0';
        else
            snprintf(err_msg, sizeof err_msg, "No se pudo conectar a %s:%d", host_buf, port_val);
    }

    if (err_msg[0])
        ImGui::TextColored({1,.3f,.3f,1}, "%s", err_msg);

    ImGui::End();
}

// ── Ventana principal ─────────────────────────────────────────────────
static void window_main() {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("rat-lab##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_MenuBar);

    // ── Barra de menú ────────────────────────────────────────────────
    if (ImGui::BeginMenuBar()) {
        ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.f}, "rat-lab");
        ImGui::Separator();
        ImGui::TextDisabled("conectado");
        ImGui::Spacing();
        if (ImGui::MenuItem("Desconectar")) do_disconnect();
        ImGui::EndMenuBar();
    }

    // ── Pestañas ─────────────────────────────────────────────────────
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Chat"))      { tab_chat();      ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Shell"))     { tab_shell();     ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Sistema"))   { tab_sistema();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Procesos"))  { tab_procesos();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Archivos"))  { tab_archivos();  ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ── main ──────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    rl_net_init();

    // Conexión rápida por argumentos: ratlab-gui <host> [puerto]
    bool auto_connect = false;
    if (argc > 1) {
        int p = argc > 2 ? atoi(argv[2]) : RL_PORT;
        auto_connect = do_connect(argv[1], p);
    }
    (void)auto_connect;

    // Inicializar GLFW
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *win = glfwCreateWindow(900, 620, "rat-lab", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1); // VSync

    // Inicializar ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Estilo oscuro
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding   = 4.f;
    ImGui::GetStyle().FrameRounding    = 3.f;
    ImGui::GetStyle().TabRounding      = 3.f;
    ImGui::GetStyle().ScrollbarRounding= 3.f;

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Cargar fuente predeterminada con tamaño ligeramente mayor
    io.Fonts->AddFontDefault();

    // Bucle principal
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (g_fd < 0 || !g_running)
            window_connect();
        else
            window_main();

        ImGui::Render();
        int W, H; glfwGetFramebufferSize(win, &W, &H);
        glViewport(0, 0, W, H);
        glClearColor(0.1f, 0.1f, 0.1f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    do_disconnect();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    rl_net_cleanup();
    return 0;
}
