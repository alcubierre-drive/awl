#include "cal_eds.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The types the functions below take, opaque but for GError and GList,
 * whose layout is GLib's stable public API: building needs neither EDS's
 * headers nor GLib's. CAL_ECAL_CHECK (make ECAL_CHECK=1) builds against the
 * real headers instead and checks every prototype against them. */
#ifdef CAL_ECAL_CHECK
#include <libecal/libecal.h>
#else
typedef int gboolean;
typedef struct _GCancellable GCancellable;
typedef struct _GError {
    uint32_t domain;
    int code;
    char *message;
} GError;
typedef struct _GList GList;
struct _GList {
    void *data;
    GList *next, *prev;
};
typedef void (*GDestroyNotify)(void *data);
typedef struct _GMainContext GMainContext;
typedef struct _ESourceRegistry ESourceRegistry;
typedef struct _ESource ESource;
typedef struct _ESourceSelectable ESourceSelectable;
typedef struct _EClient EClient;
typedef struct _ECalClient ECalClient;
typedef unsigned ECalClientSourceType;
typedef struct _ICalTime ICalTime;
typedef struct _ICalTimezone ICalTimezone;
typedef struct _ICalComponent ICalComponent;
typedef gboolean (*ECalRecurInstanceCb)(ICalComponent *icomp, ICalTime *instance_start,
                                        ICalTime *instance_end, void *user_data,
                                        GCancellable *cancellable, GError **error);
#define E_SOURCE_EXTENSION_CALENDAR "Calendar"
#define E_CAL_CLIENT_SOURCE_TYPE_EVENTS 0
#endif

/* Everything used from libecal, libedataserver, libical-glib and GLib.
 * dlsym() on libecal's handle searches the libraries it pulls in too, so
 * this one handle finds them all; GLib's are the instance the process has
 * already. */
#define CAL_SYMS(X) \
    X(ESourceRegistry *, e_source_registry_new_sync, (GCancellable *, GError **)) \
    X(GList *, e_source_registry_list_enabled, (ESourceRegistry *, const char *)) \
    X(void *, e_source_get_extension, (ESource *, const char *)) \
    X(gboolean, e_source_selectable_get_selected, (ESourceSelectable *)) \
    X(char *, e_source_selectable_dup_color, (ESourceSelectable *)) \
    X(const char *, e_source_get_uid, (ESource *)) \
    X(const char *, e_source_get_display_name, (ESource *)) \
    X(EClient *, e_cal_client_connect_sync, \
      (ESource *, ECalClientSourceType, uint32_t, GCancellable *, GError **)) \
    X(ICalTimezone *, e_cal_util_get_system_timezone, (void)) \
    X(void, e_cal_client_set_default_timezone, (ECalClient *, ICalTimezone *)) \
    X(void, e_cal_client_generate_instances_sync, \
      (ECalClient *, time_t, time_t, GCancellable *, ECalRecurInstanceCb, void *)) \
    X(gboolean, i_cal_time_is_date, (const ICalTime *)) \
    X(ICalTime *, i_cal_time_convert_to_zone, (const ICalTime *, ICalTimezone *)) \
    X(int, i_cal_time_get_year, (const ICalTime *)) \
    X(int, i_cal_time_get_month, (const ICalTime *)) \
    X(int, i_cal_time_get_day, (const ICalTime *)) \
    X(int, i_cal_time_get_hour, (const ICalTime *)) \
    X(int, i_cal_time_get_minute, (const ICalTime *)) \
    X(const char *, i_cal_component_get_summary, (ICalComponent *)) \
    X(const char *, i_cal_component_get_location, (ICalComponent *)) \
    X(void, g_object_unref, (void *)) \
    X(void, g_free, (void *)) \
    X(void, g_list_free_full, (GList *, GDestroyNotify)) \
    X(void, g_error_free, (GError *)) \
    X(GCancellable *, g_cancellable_new, (void)) \
    X(void, g_cancellable_cancel, (GCancellable *)) \
    X(GMainContext *, g_main_context_new, (void)) \
    X(void, g_main_context_push_thread_default, (GMainContext *)) \
    X(void, g_main_context_pop_thread_default, (GMainContext *)) \
    X(gboolean, g_main_context_iteration, (GMainContext *, gboolean)) \
    X(void, g_main_context_unref, (GMainContext *))

#define CAL_MEMBER(ret, name, args) ret(*name) args;
static struct syms {
    CAL_SYMS(CAL_MEMBER)
} S;

#ifdef CAL_ECAL_CHECK
#define CAL_CHECK(ret, name, args) \
    _Static_assert(__builtin_types_compatible_p(__typeof__(&name), __typeof__(S.name)), \
                   #name " doesn't match its header");
CAL_SYMS(CAL_CHECK)
#endif

/* Loaded once per process and kept: the outcome stays, as does the handle
 * (RTLD_NODELETE, never dlclose()d). */
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;
static int loaded;
static cal_status_t load_status;
static char load_error[256];

static cal_status_t load(void) {
    static const char *const sonames[] = {
        "libecal-2.0.so.3", "libecal-2.0.so.2", "libecal-2.0.so.1", "libecal-2.0.so",
    };
    pthread_mutex_lock(&load_lock);
    if (loaded) goto out;
    loaded = 1;
    const char *env = getenv("AWL_ECAL_LIBRARY");
    void *h = NULL;
    if (env && *env) {
        if (strcmp(env, "none") && !(h = dlopen(env, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE)))
            snprintf(load_error, sizeof(load_error), "no calendar events: %s", dlerror());
        else if (!h)
            snprintf(load_error, sizeof(load_error), "no calendar events: AWL_ECAL_LIBRARY is none");
    } else {
        for (size_t i = 0; !h && i < sizeof(sonames) / sizeof(*sonames); i++)
            if (!(h = dlopen(sonames[i], RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE)) && !i)
                snprintf(load_error, sizeof(load_error), "no calendar events: %s", dlerror());
    }
    if (!h) {
        load_status = CAL_ERR_NOLIB;
        goto out;
    }
    /* ISO C has no cast from object to function pointer */
#define CAL_RESOLVE(ret, name, args) \
    if (load_status == CAL_OK) { \
        void *sym = dlsym(h, #name); \
        if (sym) memcpy(&S.name, &sym, sizeof(S.name)); \
        else { \
            load_status = CAL_ERR_NOSYM; \
            snprintf(load_error, sizeof(load_error), "no calendar events: %s", dlerror()); \
        } \
    }
    load_status = CAL_OK;
    CAL_SYMS(CAL_RESOLVE)
    if (load_status != CAL_OK) memset(&S, 0, sizeof(S));
out:;
    cal_status_t s = load_status;
    pthread_mutex_unlock(&load_lock);
    return s;
}

/* a selected calendar, by its source's uid */
struct client {
    char *uid, *color;
    ECalClient *client;
    int seen;
};

struct cal_backend {
    cal_log_cb log;
    /* guards cancelled and the creation of cancel */
    pthread_mutex_t lock;
    int cancelled;
    GCancellable *cancel;
    /* the querying thread's. ctx is the thread-default main context while
     * it queries, so the registry's and the clients' signals and idles go
     * there, not to the global default one, whose owner (the tray's GTK
     * thread, which also has its Wayland connection there) must never see
     * them. */
    GMainContext *ctx;
    ESourceRegistry *registry;
    time_t registry_tried;
    struct client *clients;
    size_t nclients;
    /* the last failure logged, CAL_OK since the last success */
    cal_status_t reported;
};

static void logmsg(cal_backend_t *b, int warn, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void logmsg(cal_backend_t *b, int warn, const char *fmt, ...) {
    if (!b->log) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    b->log(warn, msg);
}

/* s, logging msg once for each new cause; a missing library isn't worth a
 * warning */
static cal_status_t fail(cal_backend_t *b, cal_status_t s, const char *msg) {
    if (s == CAL_ERR_CANCELLED) return s;
    if (s != b->reported) logmsg(b, s != CAL_ERR_NOLIB, "%s", msg);
    b->reported = s;
    return s;
}

static int is_cancelled(cal_backend_t *b) {
    pthread_mutex_lock(&b->lock);
    int c = b->cancelled;
    pthread_mutex_unlock(&b->lock);
    return c;
}

static void drop_client(struct client *c) {
    if (c->client) S.g_object_unref(c->client);
    free(c->uid);
    free(c->color);
}

/* Connects to every enabled calendar that is also shown in gnome-calendar/
 * evolution ("selected"), and drops the clients whose source went away. */
static void sync_clients(cal_backend_t *b) {
    for (size_t i = 0; i < b->nclients; i++) b->clients[i].seen = 0;
    GList *sources = S.e_source_registry_list_enabled(b->registry, E_SOURCE_EXTENSION_CALENDAR);
    for (GList *l = sources; l; l = l->next) {
        ESource *src = l->data;
        ESourceSelectable *ext = S.e_source_get_extension(src, E_SOURCE_EXTENSION_CALENDAR);
        if (!S.e_source_selectable_get_selected(ext)) continue;
        const char *uid = S.e_source_get_uid(src);
        struct client *c = NULL;
        for (size_t i = 0; i < b->nclients && !c; i++)
            if (!strcmp(b->clients[i].uid, uid)) c = &b->clients[i];
        if (!c) {
            struct client *n = realloc(b->clients, (b->nclients + 1) * sizeof(*n));
            char *u = strdup(uid);
            if (!n || !u) {
                if (n) b->clients = n;
                free(u);
                continue;
            }
            b->clients = n;
            c = &b->clients[b->nclients++];
            *c = (struct client){.uid = u};
        }
        c->seen = 1;
        char *color = S.e_source_selectable_dup_color(ext);
        free(c->color);
        c->color = strdup(color ? color : "");
        (S.g_free)(color);  /* GLib has a g_free() macro */
        if (c->client) continue;
        GError *err = NULL;
        /* Don't wait for the backend to go online: cached events are what we
         * want to show right away; the factory refreshes them on its own.
         * Not 0, which waits too: for a signal on our main context, which
         * nobody iterates while this blocks. */
        EClient *client = S.e_cal_client_connect_sync(src, E_CAL_CLIENT_SOURCE_TYPE_EVENTS,
                                                      (uint32_t)-1, b->cancel, &err);
        if (!client) {
            if (!is_cancelled(b))
                logmsg(b, 1, "cannot open %s: %s", S.e_source_get_display_name(src),
                      err ? err->message : "?");
            if (err) S.g_error_free(err);
            continue;
        }
        c->client = (ECalClient *)client;
        ICalTimezone *tz = S.e_cal_util_get_system_timezone();
        if (tz) S.e_cal_client_set_default_timezone(c->client, tz);
    }
    S.g_list_free_full(sources, S.g_object_unref);
    size_t n = 0;
    for (size_t i = 0; i < b->nclients; i++) {
        if (b->clients[i].seen && b->clients[i].client) b->clients[n++] = b->clients[i];
        else drop_client(&b->clients[i]);
    }
    b->nclients = n;
}

/* Instance time -> local time_t. Timed instances carry their own zone, so
 * convert to the system zone and let mktime() do the rest (floating times
 * are local already); all-day instances only have a date. */
static time_t local_time(ICalTime *t) {
    struct tm tm = {0};
    ICalTime *lt = NULL;
    if (!S.i_cal_time_is_date(t)) {
        ICalTimezone *tz = S.e_cal_util_get_system_timezone();
        if (tz) t = lt = S.i_cal_time_convert_to_zone(t, tz);
        tm.tm_hour = S.i_cal_time_get_hour(t);
        tm.tm_min = S.i_cal_time_get_minute(t);
    }
    tm.tm_year = S.i_cal_time_get_year(t) - 1900;
    tm.tm_mon = S.i_cal_time_get_month(t) - 1;
    tm.tm_mday = S.i_cal_time_get_day(t);
    tm.tm_isdst = -1;
    if (lt) S.g_object_unref(lt);
    return mktime(&tm);
}

struct instance_ctx {
    const char *color;
    cal_event_cb cb;
    void *data;
};

static gboolean on_instance(ICalComponent *comp, ICalTime *start, ICalTime *end, void *data,
                            GCancellable *cancellable, GError **error) {
    (void)cancellable, (void)error;
    struct instance_ctx *ctx = data;
    const char *summary = S.i_cal_component_get_summary(comp);
    const char *location = S.i_cal_component_get_location(comp);
    cal_event_t e = {
        .summary = summary ? summary : "",
        .location = location ? location : "",
        .color = ctx->color,
        .all_day = !!S.i_cal_time_is_date(start),
        .start = local_time(start),
    };
    e.end = end ? local_time(end) : e.start;
    if (e.end < e.start) e.end = e.start;
    ctx->cb(&e, ctx->data);
    return 1;
}

cal_backend_t *cal_backend_open(cal_log_cb log) {
    cal_backend_t *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->log = log;
    pthread_mutex_init(&b->lock, NULL);
    return b;
}

static cal_status_t query(cal_backend_t *b, time_t start, time_t end, cal_event_cb cb, void *data);

cal_status_t cal_backend_query(cal_backend_t *b, time_t start, time_t end, cal_event_cb cb, void *data) {
    cal_status_t s = load();
    if (s != CAL_OK) return fail(b, s, load_error);
    if (!b->ctx) b->ctx = S.g_main_context_new();
    S.g_main_context_push_thread_default(b->ctx);
    /* what came in since the last query: changed sources, ... */
    while (S.g_main_context_iteration(b->ctx, 0)) {}
    s = query(b, start, end, cb, data);
    S.g_main_context_pop_thread_default(b->ctx);
    return s;
}

static cal_status_t query(cal_backend_t *b, time_t start, time_t end, cal_event_cb cb, void *data) {
    pthread_mutex_lock(&b->lock);
    if (!b->cancel) {
        b->cancel = S.g_cancellable_new();
        if (b->cancelled) S.g_cancellable_cancel(b->cancel);
    }
    int cancelled = b->cancelled;
    pthread_mutex_unlock(&b->lock);
    if (cancelled) return CAL_ERR_CANCELLED;

    if (!b->registry) {
        /* without EDS, each try may wait for D-Bus activation to time out */
        time_t now = time(NULL);
        if (b->registry_tried && now - b->registry_tried < CAL_RETRY_SECONDS)
            return CAL_ERR_NOREGISTRY;
        b->registry_tried = now;
        GError *err = NULL;
        b->registry = S.e_source_registry_new_sync(b->cancel, &err);
        if (!b->registry) {
            char msg[256];
            snprintf(msg, sizeof(msg), "no calendar events, no source registry: %s",
                     err ? err->message : "?");
            if (err) S.g_error_free(err);
            return fail(b, is_cancelled(b) ? CAL_ERR_CANCELLED : CAL_ERR_NOREGISTRY, msg);
        }
    }
    sync_clients(b);
    for (size_t i = 0; i < b->nclients && !is_cancelled(b); i++) {
        struct instance_ctx ctx = {b->clients[i].color, cb, data};
        S.e_cal_client_generate_instances_sync(b->clients[i].client, start, end, b->cancel,
                                               on_instance, &ctx);
    }
    if (is_cancelled(b)) return CAL_ERR_CANCELLED;
    b->reported = CAL_OK;
    return CAL_OK;
}

void cal_backend_cancel(cal_backend_t *b) {
    pthread_mutex_lock(&b->lock);
    b->cancelled = 1;
    if (b->cancel) S.g_cancellable_cancel(b->cancel);
    pthread_mutex_unlock(&b->lock);
}

void cal_backend_close(cal_backend_t *b) {
    if (!b) return;
    /* anything here means the library is loaded; what the objects leave
     * behind in their context goes with it */
    if (b->ctx) S.g_main_context_push_thread_default(b->ctx);
    for (size_t i = 0; i < b->nclients; i++) drop_client(&b->clients[i]);
    free(b->clients);
    if (b->registry) S.g_object_unref(b->registry);
    if (b->cancel) S.g_object_unref(b->cancel);
    if (b->ctx) {
        while (S.g_main_context_iteration(b->ctx, 0)) {}
        S.g_main_context_pop_thread_default(b->ctx);
        S.g_main_context_unref(b->ctx);
    }
    pthread_mutex_destroy(&b->lock);
    free(b);
}

const char *cal_status_str(cal_status_t s) {
    switch (s) {
    case CAL_OK: return "ok";
    case CAL_ERR_NOLIB: return "no libecal";
    case CAL_ERR_NOSYM: return "libecal lacks a function";
    case CAL_ERR_NOREGISTRY: return "no EDS source registry";
    case CAL_ERR_CANCELLED: return "cancelled";
    }
    return "?";
}
