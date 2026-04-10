CC = gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter \
         $(shell pkg-config --cflags wayland-client libavformat libavcodec libavutil libva libva-drm libdrm)
LDFLAGS = $(shell pkg-config --libs wayland-client libavformat libavcodec libavutil libva libva-drm libdrm)

TARGET = mwlw
SRCS = mwlw.c \
       wlr-layer-shell-client.c \
       linux-dmabuf-client.c \
       viewporter-client.c \
       xdg-shell-client.c \
       color-representation-client.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

PREFIX ?= /usr/local

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm755 mwlw-selector $(DESTDIR)$(PREFIX)/bin/mwlw-selector

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/bin/mwlw-selector

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean install uninstall
