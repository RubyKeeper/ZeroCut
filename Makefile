CC ?= gcc
CFLAGS ?= -O2 -Wall

GTK_MPV_FLAGS := $(shell pkg-config --cflags --libs gtk+-3.0 mpv)
FFMPEG_FLAGS := $(shell pkg-config --cflags --libs libavformat libavcodec libavutil)

all: ZeroCut bin/zerocut-cli

ZeroCut: src/main.c
	$(CC) $(CFLAGS) $< -o $@ $(GTK_MPV_FLAGS) -Wl,-rpath,'$$ORIGIN/lib'

bin/zerocut-cli: src/zerocut-cli.c
	mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@ $(FFMPEG_FLAGS) -Wl,-rpath,'$$ORIGIN/../lib'

clean:
	rm -f ZeroCut bin/zerocut-cli

.PHONY: all clean
