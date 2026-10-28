#!/usr/bin/env python3
"""Send a desktop notification when the WeChat tray icon starts blinking.

WeChat (StatusNotifierItem) blinks its tray icon by emitting NewIcon every
500ms while there are unread messages. Blinking is therefore detected as
"two or more NewIcon updates within a short window", and only the
idle -> blinking transition produces a notification.
"""

import argparse
import sys
import time
from collections import deque

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

SNI_IFACE = "org.kde.StatusNotifierItem"
PROPS_IFACE = "org.freedesktop.DBus.Properties"
WATCHER_BUS = "org.kde.StatusNotifierWatcher"
WATCHER_PATH = "/StatusNotifierWatcher"
WATCHER_IFACE = "org.kde.StatusNotifierWatcher"
NOTIFY_BUS = "org.freedesktop.Notifications"
NOTIFY_PATH = "/org/freedesktop/Notifications"


class BlinkDetector:
    """Turns a stream of icon-update timestamps into idle/blinking transitions."""

    def __init__(self, quiet, min_updates):
        self.quiet = quiet
        self.min_updates = min_updates
        self.events = deque()
        self.last_event = 0.0
        self.blinking = False

    def update(self, now):
        """Feed one icon update; returns True on the first update of a burst."""
        self.events.append(now)
        while self.events and now - self.events[0] > self.quiet:
            self.events.popleft()
        self.last_event = now
        if self.blinking or len(self.events) < self.min_updates:
            return False
        self.blinking = True
        return True

    def tick(self, now):
        """Returns True when an ongoing burst has ended."""
        if not self.blinking or now - self.last_event <= self.quiet:
            return False
        self.blinking = False
        self.events.clear()
        return True


class TrayWatcher:
    def __init__(self, args):
        self.args = args
        self.bus = dbus.SessionBus()
        self.detector = BlinkDetector(args.quiet, args.min_updates)
        self.item = None  # (service, path)
        self.matches = []
        self.notify_id = 0

    # ---- item discovery -------------------------------------------------
    def start(self):
        self.bus.add_signal_receiver(
            self._on_item_registered,
            signal_name="StatusNotifierItemRegistered",
            dbus_interface=WATCHER_IFACE,
            bus_name=WATCHER_BUS,
            path=WATCHER_PATH,
        )
        self.bus.add_signal_receiver(
            self._on_item_unregistered,
            signal_name="StatusNotifierItemUnregistered",
            dbus_interface=WATCHER_IFACE,
            bus_name=WATCHER_BUS,
            path=WATCHER_PATH,
        )
        self.bus.add_signal_receiver(
            self._on_action_invoked,
            signal_name="ActionInvoked",
            dbus_interface=NOTIFY_BUS,
            path=NOTIFY_PATH,
        )
        self.bus.add_signal_receiver(
            self._on_notification_closed,
            signal_name="NotificationClosed",
            dbus_interface=NOTIFY_BUS,
            path=NOTIFY_PATH,
        )
        self.bus.add_signal_receiver(
            self._on_owner_changed,
            signal_name="NameOwnerChanged",
            dbus_interface="org.freedesktop.DBus",
            bus_name="org.freedesktop.DBus",
            path="/org/freedesktop/DBus",
        )
        for entry in self._registered_items():
            self._maybe_watch(entry)
        GLib.timeout_add(200, self._on_tick)

    def _registered_items(self):
        props = dbus.Interface(
            self.bus.get_object(WATCHER_BUS, WATCHER_PATH), PROPS_IFACE
        )
        return [str(x) for x in props.Get(WATCHER_IFACE, "RegisteredStatusNotifierItems")]

    def _item_id(self, service, path):
        props = dbus.Interface(self.bus.get_object(service, path), PROPS_IFACE)
        return str(props.Get(SNI_IFACE, "Id"))

    def _maybe_watch(self, entry):
        service, _, path = entry.partition("/")
        path = "/" + path
        if not service:
            return
        try:
            item_id = self._item_id(service, path)
        except dbus.DBusException as exc:
            self.log("skip %s: %s" % (entry, exc.get_dbus_name()))
            return
        if item_id != self.args.item_id:
            return
        if self.item == (service, path):
            return
        self._unwatch()
        self.item = (service, path)
        self.detector = BlinkDetector(self.args.quiet, self.args.min_updates)
        self.matches = [
            self.bus.add_signal_receiver(
                self._on_icon,
                signal_name=name,
                dbus_interface=SNI_IFACE,
                bus_name=service,
                path=path,
            )
            for name in ("NewIcon", "NewAttentionIcon")
        ]
        self.matches.append(
            self.bus.add_signal_receiver(
                self._on_status,
                signal_name="NewStatus",
                dbus_interface=SNI_IFACE,
                bus_name=service,
                path=path,
            )
        )
        self.log("watching %s (%s)" % (entry, item_id))

    def _unwatch(self):
        for match in self.matches:
            match.remove()
        self.matches = []
        self.item = None

    # ---- signal handlers ------------------------------------------------
    def _on_item_registered(self, entry):
        self._maybe_watch(str(entry))

    def _on_item_unregistered(self, entry):
        service, _, path = str(entry).partition("/")
        if self.item and self.item[0] == service:
            self.log("item gone: %s" % entry)
            self._unwatch()

    def _on_owner_changed(self, name, old_owner, new_owner):
        if self.item and name == self.item[0] and not new_owner:
            self.log("owner vanished: %s" % name)
            self._unwatch()

    def _on_icon(self, *args):
        self._on_event()

    def _on_status(self, status):
        if str(status) == "NeedsAttention":
            self._on_event()
        else:
            self.log("status: %s" % status)

    def _on_event(self):
        now = time.monotonic()
        if self.detector.update(now):
            self.log("blinking started")
            self._notify()
        elif self.args.debug:
            self.log("icon update (blinking=%s)" % self.detector.blinking)

    def _on_tick(self):
        if self.detector.tick(time.monotonic()):
            self.log("blinking stopped")
        return True

    # ---- notification ---------------------------------------------------
    def _notify(self):
        try:
            iface = dbus.Interface(
                self.bus.get_object(NOTIFY_BUS, NOTIFY_PATH), NOTIFY_BUS
            )
            self.notify_id = iface.Notify(
                "wechat-tray-watch",
                self.notify_id,
                self.args.icon,
                self.args.summary,
                self.args.body,
                dbus.Array(["default", "打开微信"], signature="s"),
                {"urgency": dbus.Byte(1)},
                -1,
            )
        except dbus.DBusException as exc:
            self.log("notify failed: %s" % exc)

    def _on_notification_closed(self, notify_id, reason):
        if int(notify_id) == int(self.notify_id):
            self.notify_id = 0

    def _on_action_invoked(self, notify_id, action):
        if int(notify_id) != int(self.notify_id) or str(action) != "default":
            return
        if not self.item:
            return
        service, path = self.item
        try:
            sni = dbus.Interface(
                self.bus.get_object(service, path), SNI_IFACE
            )
            sni.Activate(0, 0)
            self.log("activated via notification click")
        except dbus.DBusException as exc:
            self.log("activate failed: %s" % exc)

    def log(self, message):
        print("%s %s" % (time.strftime("%H:%M:%S"), message), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--item-id", default="wechat", help="StatusNotifierItem Id to watch")
    parser.add_argument("--quiet", type=float, default=1.5,
                        help="seconds without icon updates that end a blink burst")
    parser.add_argument("--min-updates", type=int, default=2,
                        help="icon updates needed to count as blinking")
    parser.add_argument("--summary", default="微信通知")
    parser.add_argument("--body", default="收到新的微信消息")
    parser.add_argument("--icon", default="wechat-universal",
                        help="icon name or absolute path to a PNG")
    parser.add_argument("--debug", action="store_true", help="log every icon update")
    parser.add_argument("--probe", type=float, metavar="SECONDS",
                        help="log for N seconds and exit (no notifications)")
    args = parser.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    watcher = TrayWatcher(args)
    watcher.start()

    if args.probe:
        watcher.args.debug = True
        loop = GLib.MainLoop()
        GLib.timeout_add(int(args.probe * 1000), lambda: (loop.quit(), False)[1])
        loop.run()
        return 0

    GLib.MainLoop().run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
