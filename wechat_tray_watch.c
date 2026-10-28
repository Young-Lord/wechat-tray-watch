/* wechat_tray_watch.c -- notify when the WeChat tray icon starts blinking.
 *
 * WeChat's StatusNotifierItem emits NewIcon every 500ms while there are unread
 * messages, alternating between two pixmaps (measured on wechat-universal
 * 4.1.1.7). "Blinking" is therefore >= min_updates icon updates within `quiet`
 * seconds, and only the idle -> blinking edge produces a notification.
 *
 * Event driven: the burst timer is armed only while icon updates arrive, so an
 * idle daemon gets no timer wakeups at all.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#define SNI_IFACE "org.kde.StatusNotifierItem"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"
#define WATCHER_SVC "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define NOTIFY_SVC "org.freedesktop.Notifications"
#define NOTIFY_PATH "/org/freedesktop/Notifications"

typedef struct {
        sd_bus *bus;
        sd_event *loop;

        sd_event_source *quiet_timer;
        sd_bus_slot *slot_registered;   /* StatusNotifierItemRegistered */
        sd_bus_slot *slot_unregistered; /* StatusNotifierItemUnregistered */
        sd_bus_slot *slot_owner_changed;
        sd_bus_slot *slot_action;       /* ActionInvoked */
        sd_bus_slot *slot_closed;       /* NotificationClosed */
        sd_bus_slot *slot_icon;         /* current item: NewIcon */
        sd_bus_slot *slot_attention;    /* current item: NewAttentionIcon */
        sd_bus_slot *slot_status;       /* current item: NewStatus */

        char *service;                  /* watched SNI service, NULL if absent */
        char *path;

        unsigned quiet_usec;
        unsigned min_updates;
        unsigned burst;

        bool blinking;
        unsigned notify_id;             /* replaces_id for the open notification */

        const char *item_id;
        const char *summary;
        const char *body;
        const char *icon;
        bool debug;
} watcher;

static void logmsg(const char *fmt, ...)
{
        char ts[16];
        struct tm tm;
        time_t now = time(NULL);
        va_list ap;

        localtime_r(&now, &tm);
        strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
        fprintf(stderr, "%s ", ts);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
        fflush(stderr);
}

static bool streq(const char *a, const char *b)
{
        return a && b && strcmp(a, b) == 0;
}

static void unwatch(watcher *w)
{
        w->slot_icon = sd_bus_slot_unref(w->slot_icon);
        w->slot_attention = sd_bus_slot_unref(w->slot_attention);
        w->slot_status = sd_bus_slot_unref(w->slot_status);
        free(w->service);
        free(w->path);
        w->service = w->path = NULL;
        w->blinking = false;
        w->burst = 0;
        w->notify_id = 0;
}

/* ---- notification --------------------------------------------------------- */

static void send_notification(watcher *w)
{
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message *m = NULL, *reply = NULL;
        uint32_t id;
        int r;

        r = sd_bus_message_new_method_call(w->bus, &m, NOTIFY_SVC, NOTIFY_PATH, NOTIFY_SVC, "Notify");
        if (r < 0)
                goto fail;

        r = sd_bus_message_append(m, "susss", "wechat-tray-watch", w->notify_id, w->icon,
                                  w->summary, w->body);
        if (r < 0)
                goto fail;

        /* actions: flat string array, alternating key/label */
        sd_bus_message_open_container(m, 'a', "s");
        sd_bus_message_append(m, "s", "default");
        sd_bus_message_append(m, "s", "打开微信");
        sd_bus_message_close_container(m);

        sd_bus_message_open_container(m, 'a', "{sv}");
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "urgency");
        sd_bus_message_open_container(m, 'v', "y");
        sd_bus_message_append(m, "y", 1);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);

        sd_bus_message_append(m, "i", -1);

        r = sd_bus_call(w->bus, m, 0, &err, &reply);
        if (r < 0)
                goto fail;

        if (sd_bus_message_read(reply, "u", &id) >= 0)
                w->notify_id = id;
        goto out;

fail:
        logmsg("notify failed: %s", err.message ? err.message : strerror(-r));
out:
        sd_bus_error_free(&err);
        sd_bus_message_unref(m);
        sd_bus_message_unref(reply);
}

static int on_action_invoked(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
        watcher *w = userdata;
        const char *action = NULL;
        uint32_t id;

        (void)ret_error;
        if (sd_bus_message_read(m, "us", &id, &action) < 0)
                return 0;
        if (!w->service || id != w->notify_id || !streq(action, "default"))
                return 0;

        if (sd_bus_call_method(w->bus, w->service, w->path, SNI_IFACE, "Activate", NULL, NULL,
                               "ii", 0, 0) < 0)
                logmsg("activate failed");
        else
                logmsg("activated via notification click");
        return 0;
}

static int on_notification_closed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
        watcher *w = userdata;
        uint32_t id, reason;

        (void)ret_error;
        if (sd_bus_message_read(m, "uu", &id, &reason) < 0)
                return 0;
        if (id == w->notify_id)
                w->notify_id = 0;
        return 0;
}

/* ---- blink detection ------------------------------------------------------ */

static int on_quiet(sd_event_source *s, uint64_t usec, void *userdata)
{
        watcher *w = userdata;

        (void)s;
        (void)usec;
        if (w->blinking)
                logmsg("blinking stopped");
        w->blinking = false;
        w->burst = 0;
        return 0;
}

static void arm_quiet_timer(watcher *w)
{
        uint64_t now;

        if (sd_event_now(w->loop, CLOCK_MONOTONIC, &now) < 0)
                return;
        sd_event_source_set_time(w->quiet_timer, now + w->quiet_usec);
        sd_event_source_set_enabled(w->quiet_timer, SD_EVENT_ONESHOT);
}

static void burst_update(watcher *w)
{
        w->burst++;

        if (!w->blinking && w->burst >= w->min_updates) {
                w->blinking = true;
                logmsg("blinking started");
                send_notification(w);
        } else if (w->debug) {
                logmsg("icon update (blinking=%s)", w->blinking ? "true" : "false");
        }

        arm_quiet_timer(w);
}

static int on_icon_update(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
        (void)m;
        (void)ret_error;
        burst_update(userdata);
        return 0;
}

static int on_status(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
        watcher *w = userdata;
        const char *status = NULL;

        (void)ret_error;
        if (sd_bus_message_read(m, "s", &status) < 0)
                return 0;
        if (streq(status, "NeedsAttention"))
                burst_update(w);
        else if (w->debug)
                logmsg("status: %s", status);
        return 0;
}

/* ---- item discovery ------------------------------------------------------- */

static int get_string_prop(sd_bus *bus, const char *service, const char *path,
                           const char *iface, const char *prop, char **ret)
{
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        const char *value;
        int r;

        r = sd_bus_call_method(bus, service, path, PROPS_IFACE, "Get", &err, &reply,
                               "ss", iface, prop);
        if (r < 0)
                goto out;
        r = sd_bus_message_enter_container(reply, 'v', NULL);
        if (r < 0)
                goto out;
        r = sd_bus_message_read(reply, "s", &value);
        if (r < 0)
                goto out;
        *ret = strdup(value);
        r = *ret ? 0 : -1;
out:
        sd_bus_error_free(&err);
        sd_bus_message_unref(reply);
        return r < 0 ? r : 0;
}

static void maybe_watch(watcher *w, const char *entry)
{
        char *service = NULL, *path = NULL, *id = NULL;
        const char *slash;

        slash = strchr(entry, '/');
        if (!slash)
                return;
        service = strndup(entry, (size_t) (slash - entry));
        path = strdup(slash);
        if (!service || !path)
                goto out;

        if (get_string_prop(w->bus, service, path, SNI_IFACE, "Id", &id) < 0) {
                if (w->debug)
                        logmsg("skip %s (no Id)", entry);
                goto out;
        }
        if (!streq(id, w->item_id))
                goto out;
        if (streq(w->service, service))
                goto out;

        unwatch(w);
        w->service = service;
        w->path = path;
        service = path = NULL;

        sd_bus_match_signal(w->bus, &w->slot_icon, w->service, w->path, SNI_IFACE,
                            "NewIcon", on_icon_update, w);
        sd_bus_match_signal(w->bus, &w->slot_attention, w->service, w->path, SNI_IFACE,
                            "NewAttentionIcon", on_icon_update, w);
        sd_bus_match_signal(w->bus, &w->slot_status, w->service, w->path, SNI_IFACE,
                            "NewStatus", on_status, w);
        logmsg("watching %s (%s)", entry, id);
out:
        free(service);
        free(path);
        free(id);
}

static void scan_items(watcher *w)
{
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        char **items = NULL;

        if (sd_bus_call_method(w->bus, WATCHER_SVC, WATCHER_PATH, PROPS_IFACE, "Get", &err,
                               &reply, "ss", WATCHER_IFACE,
                               "RegisteredStatusNotifierItems") < 0) {
                logmsg("cannot list tray items: %s", err.message ? err.message : "?");
                goto out;
        }
        if (sd_bus_message_enter_container(reply, 'v', NULL) < 0)
                goto out;
        if (sd_bus_message_read_strv(reply, &items) < 0)
                goto out;

        for (char **p = items; p && *p; p++)
                maybe_watch(w, *p);
out:
        for (char **p = items; p && *p; p++)
                free(*p);
        free(items);
        sd_bus_error_free(&err);
        sd_bus_message_unref(reply);
}

static int on_item_registered(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
        const char *entry = NULL;

        (void)ret_error;
        if (sd_bus_message_read(m, "s", &entry) >= 0)
                maybe_watch(userdata, entry);
        return 0;
}

static int on_item_unregistered(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
        watcher *w = userdata;
        const char *entry = NULL;
        const char *slash;

        (void)ret_error;
        if (sd_bus_message_read(m, "s", &entry) < 0 || !w->service)
                return 0;
        slash = strchr(entry, '/');
        if (slash && (size_t) (slash - entry) == strlen(w->service) &&
            strncmp(entry, w->service, (size_t) (slash - entry)) == 0) {
                logmsg("item gone: %s", entry);
                unwatch(w);
        }
        return 0;
}

static int on_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
        watcher *w = userdata;
        const char *name, *old_owner, *new_owner;

        (void)ret_error;
        if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
                return 0;
        if (w->service && new_owner[0] == '\0' && streq(name, w->service)) {
                logmsg("owner vanished: %s", name);
                unwatch(w);
        }
        return 0;
}

/* ---- main ----------------------------------------------------------------- */

static void usage(FILE *f)
{
        fprintf(f,
                "usage: wechat_tray_watch [options]\n"
                "  --item-id ID        StatusNotifierItem Id to watch (default: wechat)\n"
                "  --quiet SECS        silence that ends a blink burst (default: 1.5)\n"
                "  --min-updates N     icon updates that count as blinking (default: 2)\n"
                "  --summary TEXT      notification title (default: 微信通知)\n"
                "  --body TEXT         notification body (default: 收到新的微信消息)\n"
                "  --icon NAME|PATH    icon name or absolute PNG (default: wechat-universal)\n"
                "  --debug             log every icon update\n"
                "  --probe SECS        observe for N seconds, then exit\n"
                "  --test              send one notification and exit\n");
}

static int on_probe_timeout(sd_event_source *s, uint64_t usec, void *userdata)
{
        (void)s;
        (void)usec;
        sd_event_exit(userdata, 0);
        return 0;
}

int main(int argc, char *argv[])
{
        watcher w = {
                .item_id = "wechat",
                .quiet_usec = 1500000,
                .min_updates = 2,
                .summary = "微信通知",
                .body = "收到新的微信消息",
                .icon = "wechat-universal",
        };
        double probe = 0;
        uint64_t now;
        int r;

        for (int i = 1; i < argc; i++) {
                const char *arg = argv[i];
                const char *val = i + 1 < argc ? argv[i + 1] : NULL;

                if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
                        usage(stdout);
                        return 0;
                } else if (!strcmp(arg, "--debug")) {
                        w.debug = true;
                } else if (!strcmp(arg, "--test")) {
                        probe = -1;
                } else if (!strcmp(arg, "--item-id") && val) {
                        w.item_id = val;
                        i++;
                } else if (!strcmp(arg, "--quiet") && val) {
                        w.quiet_usec = (unsigned) (strtod(val, NULL) * 1000000);
                        i++;
                } else if (!strcmp(arg, "--min-updates") && val) {
                        w.min_updates = (unsigned) strtoul(val, NULL, 10);
                        i++;
                } else if (!strcmp(arg, "--summary") && val) {
                        w.summary = val;
                        i++;
                } else if (!strcmp(arg, "--body") && val) {
                        w.body = val;
                        i++;
                } else if (!strcmp(arg, "--icon") && val) {
                        w.icon = val;
                        i++;
                } else if (!strcmp(arg, "--probe") && val) {
                        probe = strtod(val, NULL);
                        i++;
                } else {
                        usage(stderr);
                        return 2;
                }
        }
        if (w.min_updates == 0)
                w.min_updates = 1;

        r = sd_event_new(&w.loop);
        if (r < 0) {
                logmsg("cannot create event loop: %s", strerror(-r));
                return 1;
        }
        r = sd_bus_open_user(&w.bus);
        if (r < 0) {
                logmsg("cannot connect to session bus: %s", strerror(-r));
                return 1;
        }
        sd_bus_attach_event(w.bus, w.loop, SD_EVENT_PRIORITY_NORMAL);

        if (probe < 0) { /* --test */
                send_notification(&w);
                return 0;
        }

        sd_event_add_time(w.loop, &w.quiet_timer, CLOCK_MONOTONIC, 0, 0, on_quiet, &w);
        sd_event_source_set_enabled(w.quiet_timer, SD_EVENT_OFF);

        sd_bus_match_signal(w.bus, &w.slot_registered, WATCHER_SVC, WATCHER_PATH, WATCHER_IFACE,
                            "StatusNotifierItemRegistered", on_item_registered, &w);
        sd_bus_match_signal(w.bus, &w.slot_unregistered, WATCHER_SVC, WATCHER_PATH, WATCHER_IFACE,
                            "StatusNotifierItemUnregistered", on_item_unregistered, &w);
        sd_bus_match_signal(w.bus, &w.slot_owner_changed, "org.freedesktop.DBus",
                            "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                            on_owner_changed, &w);
        sd_bus_match_signal(w.bus, &w.slot_action, NOTIFY_SVC, NOTIFY_PATH, NOTIFY_SVC,
                            "ActionInvoked", on_action_invoked, &w);
        sd_bus_match_signal(w.bus, &w.slot_closed, NOTIFY_SVC, NOTIFY_PATH, NOTIFY_SVC,
                            "NotificationClosed", on_notification_closed, &w);

        scan_items(&w);

        if (probe > 0) {
                w.debug = true;
                sd_event_now(w.loop, CLOCK_MONOTONIC, &now);
                sd_event_add_time(w.loop, NULL, CLOCK_MONOTONIC, now + (uint64_t) (probe * 1000000),
                                  0, on_probe_timeout, w.loop);
        }

        r = sd_event_loop(w.loop);
        if (r < 0)
                logmsg("event loop failed: %s", strerror(-r));

        unwatch(&w);
        w.quiet_timer = sd_event_source_unref(w.quiet_timer);
        w.slot_registered = sd_bus_slot_unref(w.slot_registered);
        w.slot_unregistered = sd_bus_slot_unref(w.slot_unregistered);
        w.slot_owner_changed = sd_bus_slot_unref(w.slot_owner_changed);
        w.slot_action = sd_bus_slot_unref(w.slot_action);
        w.slot_closed = sd_bus_slot_unref(w.slot_closed);
        w.bus = sd_bus_flush_close_unref(w.bus);
        w.loop = sd_event_unref(w.loop);
        return r < 0 ? 1 : 0;
}
