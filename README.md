# rat-lab

Herramienta de acceso remoto para red doméstica propia. Alternativa autoalojada a AnyDesk/TeamViewer para administrar equipos Linux y Windows en la misma LAN, sin depender de servicios externos.

## Características

- **Shell remota**: ejecuta comandos en el servidor y recibe la salida
- **Transferencia de archivos**: subir y bajar archivos en chunks (sin límite de tamaño)
- **Info del sistema**: SO, uptime, memoria
- **Gestión de procesos**: listar y terminar procesos remotamente
- **Chat**: mensajes broadcast entre todos los clientes conectados
- **Multi-cliente**: el servidor acepta conexiones simultáneas (un hilo por cliente)
- **Cross-platform**: servidor y cliente REPL funcionan en Linux y Windows

## Clientes disponibles

| Cliente | Archivo | Plataforma |
|---|---|---|
| REPL (texto) | `client.c` | Linux + Windows |
| TUI (ncurses) | `client-tui.c` | Linux |
| GUI (Dear ImGui) | `client-gui.cpp` | Linux |

## Requisitos

### Linux
```bash
# Para compilar la GUI
sudo dnf install -y glfw-devel mesa-libGL-devel   # Fedora
sudo apt install -y libglfw3-dev libgl1-mesa-dev  # Debian/Ubuntu

# ncurses suele venir preinstalada
```

### Cross-compilar para Windows (desde Linux)
```bash
sudo dnf install -y mingw64-gcc   # Fedora
sudo apt install -y gcc-mingw-w64 # Debian/Ubuntu
```

## Compilar

```bash
# Todo (Linux): servidor + cliente REPL + TUI + GUI
make

# Solo Windows (cross-compile)
make windows

# Binario individual
make ratlab-server
make ratlab-client
make ratlab-tui
make ratlab-gui
```

## Uso

```bash
# Servidor — escucha en el puerto 44445 por defecto
./ratlab-server
./ratlab-server 5555          # puerto personalizado

# Cliente REPL
./ratlab-client 192.168.1.X

# Cliente TUI (interfaz ncurses)
./ratlab-tui 192.168.1.X

# Cliente GUI (ventana Dear ImGui)
./ratlab-gui                  # abre diálogo de conexión
./ratlab-gui 192.168.1.X
```

### Comandos disponibles (REPL y TUI)

```
ping                  latido — el servidor responde con su versión
shell <cmd>           ejecuta un comando en el servidor
get <remoto> <local>  descarga un archivo del servidor
put <local> <remoto>  sube un archivo al servidor
sysinfo               info del sistema del servidor
ps                    lista procesos del servidor
kill <pid>            termina un proceso (SIGKILL)
chat <mensaje>        difunde un mensaje a todos los clientes
recv                  muestra mensajes/chat pendientes (solo REPL)
quit                  desconectar
```

## Protocolo

Cada mensaje tiene el formato:

```
TIPO size\n
<size bytes de payload>
```

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
| `OK` | S→C | confirmación |
| `ERR` | S→C | error con descripción |
| `INFO` | S→C | aviso broadcast (conexión/desconexión) |

## Estructura del proyecto

```
rat-lab/
├── compat.h          abstracciones Linux/Windows (sockets, Winsock2)
├── protocol.h        protocolo de mensajes (funciones inline)
├── server.c          servidor multi-hilo (Linux + Windows)
├── client.c          cliente REPL de texto
├── client-tui.c      cliente TUI con ncurses
├── client-gui.cpp    cliente GUI con Dear ImGui + GLFW + OpenGL3
├── Makefile
└── imgui/            Dear ImGui v1.93.0 (vendoreado)
```

## Aviso de seguridad

Esta herramienta está diseñada para uso **exclusivo en red doméstica propia**. Actualmente no implementa autenticación ni cifrado — cualquier equipo de la LAN que conozca el puerto puede conectarse. No expongas el puerto a Internet.

Funciones planeadas: autenticación por token y cifrado del canal (TLS o Noise Protocol).

## Licencia

MIT
