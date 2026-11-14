# SPDX-License-Identifier: MIT
# libvdpau_sun4i-drm: VDPAU driver for the A10/A20 (cedrus + sun4i-drm). Native build on the board.
# CPUFLAGS: A20 (Cortex-A7) by default; for the A10 use CPUFLAGS="-mcpu=cortex-a8 -mfpu=neon".
TARGET = libvdpau_sun4i-drm.so.1
SRC = device.c handles.c surface_video.c decoder.c h264.c drm.c surface_output.c mixer.c \
	presentation_queue.c
CFLAGS ?= -O2 -g
CPUFLAGS ?= -mcpu=cortex-a7 -mfpu=neon-vfpv4
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fPIC -fvisibility=hidden \
	$(CPUFLAGS) $(shell pkg-config --cflags vdpau x11 x11-xcb xcb xcb-dri2 libdrm)
LDLIBS = $(shell pkg-config --libs x11 x11-xcb xcb xcb-dri2 libdrm) -lpthread -lm
LIBDIR ?= /usr/lib/arm-linux-gnueabihf/vdpau

$(TARGET): $(SRC:.c=.o)
	$(CC) -shared -Wl,-soname,$(TARGET) -Wl,--no-undefined -o $@ $^ $(LDLIBS)

%.o: %.c vdpau_private.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -D -m 644 $(TARGET) $(DESTDIR)$(LIBDIR)/$(TARGET)
	ln -sf $(TARGET) $(DESTDIR)$(LIBDIR)/libvdpau_sun4i-drm.so

clean:
	rm -f *.o $(TARGET)

.PHONY: install clean
