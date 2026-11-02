# Balloon VR — Linux build.
#
# With distro packages:
#   sudo apt install build-essential pkg-config libraylib-dev libopenxr-dev \
#       libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev libxinerama-dev \
#       libxcursor-dev libxext-dev libasound2-dev
#   make
#
# Without them, fetch raylib 5.5 and the OpenXR loader into third_party/:
#   ./scripts/bootstrap-deps.sh
#   make
#
# Overrides: make RAYLIB_DIR=... OPENXR_DIR=...
#   RAYLIB_DIR  directory that contains raylib.h and libraylib.a
#   OPENXR_DIR  prefix with include/openxr and lib/libopenxr_loader.so

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -O2
LDFLAGS ?=

APP     := balloon-vr
SRC     := src/main.c src/wind.c src/balloon.c src/handles.c src/terrain.c \
           src/terrain_mesh.c src/scene.c src/watch.c src/xr_glx.c src/xr_session.c

ifeq ($(RAYLIB_DIR),)
  ifneq ($(wildcard third_party/raylib/src/libraylib.a),)
    RAYLIB_DIR := third_party/raylib/src
  endif
endif

ifeq ($(OPENXR_DIR),)
  ifneq ($(wildcard third_party/install/include/openxr/openxr.h),)
    OPENXR_DIR := third_party/install
  endif
endif

SYS_LIBS := -lGL -lX11 -lXrandr -lXinerama -lXi -lXcursor -lXrender -lXfixes -lXext \
            -lasound -lpthread -ldl -lrt -lm

ifneq ($(RAYLIB_DIR),)
  CFLAGS += -I$(RAYLIB_DIR)
  RAYLIB_LIBS := $(RAYLIB_DIR)/libraylib.a
else
  CFLAGS += $(shell pkg-config --cflags raylib 2>/dev/null)
  RAYLIB_LIBS := $(shell pkg-config --libs raylib 2>/dev/null)
endif

ifneq ($(OPENXR_DIR),)
  CFLAGS += -I$(OPENXR_DIR)/include
  OPENXR_LIBS := -L$(OPENXR_DIR)/lib -lopenxr_loader -Wl,-rpath,$(OPENXR_DIR)/lib
else
  CFLAGS += $(shell pkg-config --cflags openxr 2>/dev/null)
  OPENXR_LIBS := $(shell pkg-config --libs openxr 2>/dev/null)
endif

.PHONY: all test clean deps

all: $(APP)

$(APP): $(SRC)
	@test -n "$(RAYLIB_LIBS)" || { echo "raylib not found. Install libraylib-dev or run ./scripts/bootstrap-deps.sh"; exit 1; }
	@test -n "$(OPENXR_LIBS)" || { echo "OpenXR loader not found. Install libopenxr-dev or run ./scripts/bootstrap-deps.sh"; exit 1; }
	$(CC) $(CFLAGS) -o $@ $(SRC) $(RAYLIB_LIBS) $(OPENXR_LIBS) $(SYS_LIBS) $(LDFLAGS)

test: tests/test_sim
	./tests/test_sim

tests/test_sim: tests/test_sim.c src/wind.c src/balloon.c src/handles.c src/terrain.c src/wind.h src/balloon.h src/handles.h src/terrain.h
	$(CC) $(CFLAGS) -o $@ tests/test_sim.c src/wind.c src/balloon.c src/handles.c src/terrain.c -lm

deps:
	./scripts/bootstrap-deps.sh

clean:
	rm -f $(APP) tests/test_sim
