# rat-lab — contexto del proyecto

## Qué es esto
Herramienta de acceso remoto / soporte para **red casera propia**, similar a AnyDesk pero sin pagar servicios de terceros. Funciona solo en la LAN del usuario (no hay relay externo). El usuario tiene equipos Linux y Windows.

**No es malware.** Es una herramienta de administración para máquinas propias. No ayudar con evasión de antivirus, persistencia encubierta ni despliegue contra equipos ajenos.

---

## Estado actual (nov 2026)

### Qué está hecho y funciona
| Componente | Archivo | Estado |
|---|---|---|
| Protocolo TCP propio | `protocol.h` | ✅ |
| Servidor cross-platform | `server.c` | ✅ Linux + Windows |
| Cliente REPL (texto) | `client.c` | ✅ Linux + Windows |
| Cliente TUI (ncurses) | `client-tui.c` | ✅ Linux |
| Cliente GUI (Dear ImGui) | `client-gui.cpp` | ✅ Linux |
| Abstracción plataforma | `compat.h` | ✅ |
| ImGui vendoreado | `imgui/` | ✅ v1.93.0 |

### Bugs corregidos en esta sesión
1. **Race condition** en uploads: `g_pending_upload` global → `client_ctx_t` por hilo
2. **Buffer fijo** en `drain_pending`: ahora dinámico con `realloc`
3. **Archivos grandes en RAM**: upload en chunks `FILECHUNK`/`FILEEND` (64 KB c/u)
4. **`ftell` límite 2 GB**: reemplazado por `fseeko`/`ftello` con `off_t`/`long long`

### Última verificación (2026-11-04)
- `make clean && make` compila los 4 binarios sin warnings (gcc 16, ncurses, GLFW+GL, ImGui 1.93.0)
- Smoke test OK: ping, shell, subida/bajada de archivo de 300 KB con chunks — bytes idénticos (`cmp`)
- Análisis REA previo (versión inicial del server): ELF x86-64, strings del protocolo y dispatcher decompilado con Ghidra vistos OK
- **Conocido/sin fix**: si el puerto está ocupado el server falla en `bind` con "Address already in use" (loggeado, no hay reintento)
- Estado: **estable, listo para retomar**. Pendiente principal: auth + cifrado (ver "Hacia dónde va").

---

## Protocolo de mensajes

Formato: `TIPO size\n` + `size` bytes de payload.

| Tipo | Dirección | Descripción |
|---|---|---|
| `PING` | C→S | latido |
| `PONG` | S→C | respuesta con versión |
| `SHELL <cmd>` | C→S | ejecutar comando |
| `DATA` | S→C | resultado de SHELL / SYSINFO / PS |
| `SYSINFO` | C→S | info del sistema |
| `PS` | C→S | lista de procesos |
| `KILL <pid>` | C→S | terminar proceso |
| `CHAT <msg>` | C→S | mensaje broadcast |
| `DOWNLOAD <ruta>` | C→S | pedir archivo |
| `FILEDATA <size>` | S→C | contenido del archivo |
| `UPLOAD <ruta>` | C→S | avisar que viene archivo |
| `FILECHUNK` | C→S | trozo de archivo (64 KB) |
| `FILEEND` | C→S | fin de archivo |
| `FILE` | C→S | archivo completo (legado) |
| `OK` | S→C | confirmación |
| `ERR` | S→C | error con descripción |
| `INFO` | S→C | aviso broadcast (conexión/desconexión) |

---

## Compilar

```bash
# Todo Linux (server + client REPL + TUI + GUI)
make

# Solo el servidor para Windows (cross-compile desde Linux)
# Requiere: sudo dnf install mingw64-gcc
make windows

# Individual
make ratlab-server
make ratlab-client
make ratlab-tui     # requiere ncurses
make ratlab-gui     # requiere glfw-devel mesa-libGL-devel
```

### Dependencias Linux
```bash
sudo dnf install -y glfw-devel mesa-libGL-devel  # para GUI
# ncurses ya viene instalada normalmente
```

### Dependencias Windows (cross-compile)
```bash
sudo dnf install -y mingw64-gcc   # Fedora
# sudo apt install gcc-mingw-w64  # Debian/Ubuntu
```

Los `.exe` generados no necesitan instalador en Windows (pthreads estáticos incluidos).

---

## Uso rápido

```bash
# Servidor (Linux o Windows)
./ratlab-server              # puerto 44445 por defecto
./ratlab-server 5555         # puerto personalizado

# Cliente REPL
./ratlab-client 192.168.1.X

# Cliente TUI (ncurses, Linux)
./ratlab-tui 192.168.1.X

# Cliente GUI (ventana, Linux)
./ratlab-gui 192.168.1.X
./ratlab-gui                 # abre diálogo de conexión
```

---

## Hacia dónde va el proyecto

### Prioridad alta
- [ ] **Autenticación básica**: contraseña o token compartido al conectar (ahora cualquier IP de la LAN puede entrar)
- [ ] **Cifrado del canal**: mínimo un TLS simple con certificado autofirmado, o Noise Protocol

### Prioridad media
- [ ] **GUI Windows**: la GUI usa GLFW+OpenGL y en teoría compila con MinGW, pero no se ha probado aún
- [ ] **Scroll-back en TUI**: el área de salida solo tiene auto-scroll, no permite subir con el teclado
- [ ] **Historial de comandos shell** en TUI (flecha arriba)
- [ ] **Explorador de archivos** en GUI: botón para navegar carpetas en lugar de escribir rutas a mano
- [ ] **Notificación de nueva conexión**: popup/sonido cuando un equipo se conecta al servidor

### Prioridad baja / ideas
- [ ] Capturas de pantalla remotas (screenshot del escritorio del servidor)
- [ ] Transferencia de archivos con barra de progreso en GUI
- [ ] Modo solo-chat (sin shell) para clientes de confianza reducida
- [ ] Empaquetado `.rpm` / `.deb` / installer Windows

---

## Estructura de archivos

```
rat-lab/
├── CLAUDE.md           ← este archivo
├── Makefile
├── compat.h            ← abstracciones Linux/Windows (sockets, threads)
├── protocol.h          ← protocolo de mensajes (funciones inline)
├── server.c            ← servidor multi-hilo (Linux + Windows)
├── client.c            ← cliente REPL de texto
├── client-tui.c        ← cliente TUI con ncurses
├── client-gui.cpp      ← cliente GUI con Dear ImGui + GLFW
├── imgui/              ← Dear ImGui v1.93.0 (vendoreado)
│   ├── imgui.h / .cpp
│   ├── imgui_impl_glfw.h / .cpp
│   ├── imgui_impl_opengl3.h / .cpp
│   └── ...
├── ratlab-server       ← binario Linux (generado)
├── ratlab-client       ← binario Linux (generado)
├── ratlab-tui          ← binario Linux (generado)
├── ratlab-gui          ← binario Linux (generado)
├── ratlab-server.exe   ← binario Windows (generado con make windows)
├── ratlab-client.exe   ← binario Windows (generado con make windows)
└── ratlab-server.log   ← log del servidor
```
