CC      ?= gcc
CXX     ?= g++
CFLAGS  ?= -O2 -Wall -Wextra -pthread
CXXFLAGS ?= -O2 -Wall -Wextra -pthread -std=c++17

# Cross-compilador para Windows (MinGW-w64)
WINCC   ?= x86_64-w64-mingw32-gcc
WINFLAGS = -O2 -Wall -Wextra -pthread -DWIN32_LEAN_AND_MEAN

HEADERS = protocol.h compat.h

# Fuentes de ImGui (vendored)
IMGUI_SRC = imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_widgets.cpp \
            imgui/imgui_tables.cpp \
            imgui/imgui_impl_glfw.cpp imgui/imgui_impl_opengl3.cpp

IMGUI_FLAGS = $(shell pkg-config --cflags --libs glfw3) -lGL

# --- Linux ---
all: ratlab-server ratlab-client ratlab-tui ratlab-gui

ratlab-server: server.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ server.c

ratlab-client: client.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ client.c

ratlab-tui: client-tui.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ client-tui.c $(shell pkg-config --libs ncurses)

ratlab-gui: client-gui.cpp $(IMGUI_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ client-gui.cpp $(IMGUI_SRC) $(IMGUI_FLAGS)

# --- Windows (cross-compile desde Linux) ---
# Requiere: sudo dnf install mingw64-gcc   (Fedora)
#           sudo apt install gcc-mingw-w64  (Debian/Ubuntu)
windows: ratlab-server.exe ratlab-client.exe

ratlab-server.exe: server.c $(HEADERS)
	$(WINCC) $(WINFLAGS) -o $@ server.c -lws2_32

ratlab-client.exe: client.c $(HEADERS)
	$(WINCC) $(WINFLAGS) -o $@ client.c -lws2_32

clean:
	rm -f ratlab-server ratlab-client ratlab-tui ratlab-gui ratlab-server.exe ratlab-client.exe

.PHONY: all windows clean
