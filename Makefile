CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
PKG := libsystemd

wechat_tray_watch: wechat_tray_watch.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(PKG)) -o $@ $< $(shell pkg-config --libs $(PKG))

clean:
	rm -f wechat_tray_watch

.PHONY: clean
