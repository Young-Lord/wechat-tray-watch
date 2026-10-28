# wechat-tray-watch

Sends a desktop notification the moment the WeChat tray icon starts blinking
(i.e. when there are unread messages), and only on that transition:
idle → blinking notifies once, blinking → idle stays silent.

## How detection works

On KDE Plasma (Wayland or X11) the WeChat tray icon is a `StatusNotifierItem`
(`Id=wechat`, session-bus name `org.kde.StatusNotifierItem-<pid>-<n>`).
Measured behaviour of `wechat-universal-bwrap` 4.1.1.7: while blinking it emits
`NewIcon` exactly every 500 ms, alternating between two icon pixmaps. Nothing
else changes — `Status` stays `Active` and `ToolTip` carries no unread text.

So the daemon watches `NewIcon` / `NewAttentionIcon` (plus `NewStatus ==
NeedsAttention` for other clients) and treats "≥2 updates within `--quiet`
seconds" as blinking. The service name is discovered through
`org.kde.StatusNotifierWatcher`, so WeChat restarts are handled automatically.

## Requirements

- KDE Plasma with a StatusNotifierItem-based tray
- C build: `gcc` + `pkg-config` + libsystemd (headers ship with the `systemd` package)
- Python version (reference implementation): `python-dbus`, `python-gobject`

## Build and run

```sh
make                                   # builds ./wechat_tray_watch (sd-bus, ~27 KB)
./wechat_tray_watch                    # run in foreground
./wechat_tray_watch --debug            # log every icon update
./wechat_tray_watch --probe 10         # observe for 10s, then exit
./wechat_tray_watch --test             # send one notification and exit
```

`./wechat_tray_watch.py` is the original Python implementation with identical
behaviour, kept for comparison (needs `python-dbus` + `python-gobject`).

The C daemon is fully event driven: the burst timer is armed only while icon
updates arrive, so an idle process never wakes up. Measured idle cost on
Plasma 6.8/Wayland: RSS ~3.3 MB, 1 thread, 7 fds, 0 CPU ticks over 30 s.

Knobs: `--item-id wechat`, `--quiet 1.5`, `--min-updates 2`,
`--summary 微信通知`, `--body "收到新的微信消息"`, `--icon wechat-universal`
(icon name or absolute PNG path). Clicking the notification activates the WeChat
tray item; a repeat blink updates the still-open notification instead of stacking.

## Install as user service

```sh
mkdir -p ~/.config/systemd/user
ln -sf ~/Projects/wechat-tray-watch/systemd/wechat-tray-watch.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now wechat-tray-watch.service
journalctl --user -u wechat-tray-watch -f
```
