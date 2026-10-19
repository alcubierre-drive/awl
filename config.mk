_VERSION = 0.8-dev
VERSION  = `git describe --tags --dirty 2>/dev/null || echo $(_VERSION)`

PKG_CONFIG = pkg-config

WLR_INCS = `$(PKG_CONFIG) --cflags wlroots-0.20`
WLR_LIBS = `$(PKG_CONFIG) --libs wlroots-0.20`

# Allow using an alternative wlroots installation
# This has to have all the includes required by wlroots, e.g:
# Assuming wlroots git repo is "${PWD}/wlroots" and you only ran "meson setup build && ninja -C build"
#WLR_INCS = -I/usr/include/pixman-1 -I/usr/include/elogind -I/usr/include/libdrm \
#	-I$(PWD)/wlroots/include
# Set -rpath to avoid using the wrong library.
#WLR_LIBS = -Wl,-rpath,$(PWD)/wlroots/build -L$(PWD)/wlroots/build -lwlroots-0.19

# Assuming you ran "meson setup --prefix ${PWD}/0.19 build && ninja -C build install"
#WLR_INCS = -I/usr/include/pixman-1 -I/usr/include/elogind -I/usr/include/libdrm \
#	-I$(PWD)/wlroots/0.19/include/wlroots-0.19
#WLR_LIBS = -Wl,-rpath,$(PWD)/wlroots/0.19/lib64 -L$(PWD)/wlroots/0.19/lib64 -lwlroots-0.19

XWAYLAND =
XLIBS =
# Uncomment to build XWayland support
XWAYLAND = -DXWAYLAND
XLIBS = xcb xcb-icccm

# awl itself only uses C99 features, but wlroots' headers use anonymous unions (C11).
# To avoid warnings about them, we do not use -std=c99 and instead of using the
# gmake default 'CC=c99', we use cc.
CC = gcc

CFLAGS = -std=c11 -Wall -Wextra -pedantic -Wno-unused-parameter
# CUSTOM: awl's compile-time switches, applied by a rebuild and restart of awl
# the lock screen's shatter effect, if your scenefx has it
CFLAGS += -DSCENEFX_HAS_SHATTER

# CFLAGS += -fsanitize=address -fsanitize=leak
# LDFLAGS += -fsanitize=address -fsanitize=leak
# LDFLAGS += -fsanitize=undefined
# CFLAGS += -ggdb
# CFLAGS += -O0
# CUSTOM: this machine's optimization flags
CFLAGS += -Ofast -march=native -mtune=native -flto
LDFLAGS += -Ofast -march=native -mtune=native -flto=12 -Wno-maybe-uninitialized

# CUSTOM: where this machine's scenefx is installed
AWL_LIBS := $(HOME)/Desktop/awl-libs
CFLAGS += -I$(AWL_LIBS)/include/scenefx-0.5
LDFLAGS += -L$(AWL_LIBS)/lib -Wl,-rpath=$(AWL_LIBS)/lib -lscenefx-0.5
