#include "calendar.hpp"

#include <gtk-layer-shell.h>
#include <gtkmm.h>

#ifdef AWL_HAVE_ECAL
#include <libecal/libecal.h>
#endif

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <signal.h>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace awl {
namespace {

struct Event {
    std::string summary, location, color;
    time_t start = 0, end = 0;  // local; for all-day events midnight to midnight
    bool all_day = false;
};

// Months are cached by year*12 + month (month 0-based, as in struct tm).
int month_key(int year, int month) { return year * 12 + month; }

time_t month_start(int key) {
    struct tm t = {};
    t.tm_year = key / 12 - 1900;
    t.tm_mon = key % 12;
    t.tm_mday = 1;
    t.tm_isdst = -1;
    return mktime(&t);
}

time_t day_start(int year, int month, int day) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month;
    t.tm_mday = day;
    t.tm_isdst = -1;
    return mktime(&t);
}

#ifdef AWL_HAVE_ECAL
// ---- EDS side -------------------------------------------------------------
// Queries run on a single worker thread (a GThreadPool with one thread, so
// they are serialized and the state below needs no lock): the EDS sync API
// talks D-Bus to evolution-source-registry/evolution-calendar-factory, which
// must not block the GTK thread. Registry and client connections are kept
// open between queries -- that is what makes reopening the popup instant.

struct Query {
    int key;
};

struct Result {
    int key;
    std::vector<Event> events;
};

struct Client {
    ECalClient *client = nullptr;
    std::string color;
};

GThreadPool *g_pool = nullptr;
GCancellable *g_cancel = nullptr;
ESourceRegistry *g_registry = nullptr;      // worker thread only
std::map<std::string, Client> g_clients;    // worker thread only, by source uid

void drop_client(Client &c) {
    if (c.client) g_object_unref(c.client);
    c.client = nullptr;
}

// Connects to every enabled calendar that is also shown in gnome-calendar/
// evolution ("selected"), and drops clients whose source went away.
void sync_clients() {
    std::set<std::string> seen;
    GList *sources = e_source_registry_list_enabled(g_registry, E_SOURCE_EXTENSION_CALENDAR);
    for (GList *l = sources; l; l = l->next) {
        auto *src = E_SOURCE(l->data);
        auto *ext = E_SOURCE_SELECTABLE(e_source_get_extension(src, E_SOURCE_EXTENSION_CALENDAR));
        if (!e_source_selectable_get_selected(ext)) continue;
        std::string uid = e_source_get_uid(src);
        seen.insert(uid);
        Client &c = g_clients[uid];
        gchar *color = e_source_selectable_dup_color(ext);
        c.color = color ? color : "";
        g_free(color);
        if (c.client) continue;
        GError *err = nullptr;
        // Don't wait for the backend to go online: cached events are what we
        // want to show right away; the factory refreshes them on its own.
        EClient *client = e_cal_client_connect_sync(src, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 0,
                                                    g_cancel, &err);
        if (!client) {
            g_warning("awl calendar: cannot open %s: %s", e_source_get_display_name(src),
                      err ? err->message : "?");
            g_clear_error(&err);
            continue;
        }
        c.client = E_CAL_CLIENT(client);
        if (ICalTimezone *tz = e_cal_util_get_system_timezone())
            e_cal_client_set_default_timezone(c.client, tz);
    }
    g_list_free_full(sources, g_object_unref);
    for (auto it = g_clients.begin(); it != g_clients.end();) {
        if (!seen.count(it->first) || !it->second.client) {
            drop_client(it->second);
            it = g_clients.erase(it);
        } else {
            ++it;
        }
    }
}

// Instance time -> local time_t. Timed instances carry their own zone, so
// convert to the system zone and let mktime() do the rest (floating times
// are local already); all-day instances only have a date.
time_t local_time(ICalTime *t) {
    struct tm tm = {};
    ICalTime *lt = nullptr;
    if (!i_cal_time_is_date(t)) {
        if (ICalTimezone *tz = e_cal_util_get_system_timezone())
            t = lt = i_cal_time_convert_to_zone(t, tz);
        tm.tm_hour = i_cal_time_get_hour(t);
        tm.tm_min = i_cal_time_get_minute(t);
    }
    tm.tm_year = i_cal_time_get_year(t) - 1900;
    tm.tm_mon = i_cal_time_get_month(t) - 1;
    tm.tm_mday = i_cal_time_get_day(t);
    tm.tm_isdst = -1;
    if (lt) g_object_unref(lt);
    return mktime(&tm);
}

struct InstanceCtx {
    Result *res;
    const std::string *color;
};

gboolean on_instance(ICalComponent *comp, ICalTime *start, ICalTime *end, gpointer data,
                     GCancellable *, GError **) {
    auto *ctx = static_cast<InstanceCtx *>(data);
    Event e;
    const char *s;
    e.summary = (s = i_cal_component_get_summary(comp)) ? s : "";
    e.location = (s = i_cal_component_get_location(comp)) ? s : "";
    e.color = *ctx->color;
    e.all_day = i_cal_time_is_date(start);
    e.start = local_time(start);
    e.end = end ? local_time(end) : e.start;
    if (e.end < e.start) e.end = e.start;
    ctx->res->events.push_back(std::move(e));
    return TRUE;
}

gboolean deliver(gpointer data);

void worker(gpointer data, gpointer) {
    std::unique_ptr<Query> q(static_cast<Query *>(data));
    auto *res = new Result{q->key, {}};
    if (!g_cancellable_is_cancelled(g_cancel)) {
        GError *err = nullptr;
        if (!g_registry) g_registry = e_source_registry_new_sync(g_cancel, &err);
        if (!g_registry) {
            g_warning("awl calendar: no source registry: %s", err ? err->message : "?");
            g_clear_error(&err);
        } else {
            sync_clients();
            time_t start = month_start(q->key), end = month_start(q->key + 1);
            for (auto &kv : g_clients) {
                InstanceCtx ctx{res, &kv.second.color};
                e_cal_client_generate_instances_sync(kv.second.client, start, end, g_cancel,
                                                     on_instance, &ctx);
            }
        }
    }
    // back to the GTK thread (default main context)
    g_idle_add(deliver, res);
}
#endif  // AWL_HAVE_ECAL

// ---- GTK side -------------------------------------------------------------

class Popup {
public:
    explicit Popup(const awl_tray_config_t &config);
    ~Popup();
    void configure(const awl_tray_config_t &config);
    void toggle(const std::string &mon, bool top);
    void show(const std::string &mon, bool top);
    void hide();
    void goToday();
    void stepDay(int n);
    void stepMonth(int n);
    void scroll(int amount);
#ifdef AWL_HAVE_ECAL
    void onResult(int key, std::vector<Event> events);
#endif

private:
    int shownKey();
    void select(int year, int month, int day);
    void request(int key);
    void refresh();
    void fitDayLabel();
    void launch(std::vector<std::string> argv);

    Glib::RefPtr<Gtk::CssProvider> css_;
    Gtk::Window win_;
    Gtk::Box vbox_{Gtk::ORIENTATION_VERTICAL, 6};
    Gtk::Calendar cal_;
    Gtk::Button today_btn_;
    Gtk::Label day_label_;
    Gtk::Button prev_btn_, next_btn_;
    Gtk::ScrolledWindow scroll_;
    Gtk::ListBox list_;
    Gtk::Box header_{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Button evo_btn_;

    std::string mon_;
    bool shown_ = false;
    std::map<int, std::vector<Event>> cache_;
    std::set<int> inflight_;
};

Popup *g_popup = nullptr;

// awl blurs what is behind the popup (scenefx, see createlayersurface()), so
// every background is cleared and only the window itself gets a faint
// tint. Adwaita paints white backgrounds on the calendar, list, rows and
// viewport -- the wildcard clears all of them (and borders/shadows) at once,
// then the few states that need a background get a translucent one back.
// Scoped to #awl-calendar: the provider is screen-wide and the tray windows
// live in the same process. The colors and sizes are config_plugins.h's
// (tray_config.calendar).

std::string make_css(const awl_tray_config_t &config) {
    const auto &c = config.calendar;
    gchar *css = g_strdup_printf(R"css(
#awl-calendar {
  background-color: %s;
  border: %dpx solid %s;
  border-radius: %dpx;
}
#awl-calendar * {
  background-color: transparent;
  background-image: none;
  border-color: transparent;
  box-shadow: none;
  text-shadow: none;
  -gtk-icon-shadow: none;
  color: %s;
}
#awl-calendar calendar:indeterminate { color: %s; }
#awl-calendar calendar.highlight { color: %s; font-weight: bold; }
#awl-calendar calendar:selected {
  background-color: %s;
  border-radius: 4px;
}
#awl-calendar button:hover { background-color: %s; }
#awl-calendar button:active { background-color: %s; }
)css",
        css_color(c.bg).c_str(), std::max(c.border_width, 0), css_color(c.border).c_str(),
        std::max(c.radius, 0), css_color(c.fg).c_str(), css_color(c.dim).c_str(),
        css_color(c.today).c_str(), css_color(c.selected).c_str(), css_color(c.hover).c_str(),
        css_color(c.pressed).c_str());
    std::string s = css;
    g_free(css);
    return s;
}

Popup::Popup(const awl_tray_config_t &config) {
    win_.set_decorated(false);
    win_.set_name("awl-calendar");
    // the window's own background is translucent (make_css()); only matters on
    // X11, Wayland surfaces always have alpha
    if (GdkVisual *visual = gdk_screen_get_rgba_visual(win_.get_screen()->gobj()))
        gtk_widget_set_visual(GTK_WIDGET(win_.gobj()), visual);
    css_ = Gtk::CssProvider::create();
    Gtk::StyleContext::add_provider_for_screen(win_.get_screen(), css_,
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_layer_init_for_window(win_.gobj());
    gtk_layer_set_layer(win_.gobj(), GTK_LAYER_SHELL_LAYER_TOP);
    // exclusive while shown (awl hands keyboard focus to keyboard-interactive
    // top-layer surfaces), so Escape closes it
    gtk_layer_set_keyboard_mode(win_.gobj(), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_layer_set_exclusive_zone(win_.gobj(), 0);
    gtk_layer_set_anchor(win_.gobj(), GTK_LAYER_SHELL_EDGE_RIGHT, true);

    cal_.set_display_options(Gtk::CALENDAR_SHOW_HEADING | Gtk::CALENDAR_SHOW_DAY_NAMES |
                             Gtk::CALENDAR_SHOW_WEEK_NUMBERS);
    day_label_.set_xalign(0.5);
    list_.set_selection_mode(Gtk::SELECTION_NONE);
    scroll_.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll_.set_propagate_natural_height(true);
    scroll_.add(list_);
    evo_btn_.set_image_from_icon_name("window-new-symbolic", Gtk::ICON_SIZE_MENU);
    evo_btn_.set_relief(Gtk::RELIEF_NONE);
    evo_btn_.set_tooltip_text("Open in Evolution");
    evo_btn_.set_valign(Gtk::ALIGN_CENTER);
    // < day >, like the calendar's own month and year headings: the label is
    // as wide as the longest day name (fitDayLabel()), so neither the arrows
    // nor the window move while it changes
    prev_btn_.set_image_from_icon_name("pan-start-symbolic", Gtk::ICON_SIZE_MENU);
    next_btn_.set_image_from_icon_name("pan-end-symbolic", Gtk::ICON_SIZE_MENU);
    for (Gtk::Button *b : {&prev_btn_, &next_btn_}) {
        b->set_relief(Gtk::RELIEF_NONE);
        b->set_valign(Gtk::ALIGN_CENTER);
    }
    prev_btn_.set_tooltip_text("Previous day");
    next_btn_.set_tooltip_text("Next day");
    prev_btn_.signal_clicked().connect([this] { stepDay(-1); });
    next_btn_.signal_clicked().connect([this] { stepDay(1); });
    header_.pack_start(prev_btn_, Gtk::PACK_SHRINK);
    header_.pack_start(day_label_, Gtk::PACK_EXPAND_WIDGET);
    header_.pack_start(next_btn_, Gtk::PACK_SHRINK);
    header_.pack_end(evo_btn_, Gtk::PACK_SHRINK);
    today_btn_.set_image_from_icon_name("view-refresh-symbolic", Gtk::ICON_SIZE_MENU);
    today_btn_.set_relief(Gtk::RELIEF_NONE);
    today_btn_.set_tooltip_text("Today");
    today_btn_.set_valign(Gtk::ALIGN_CENTER);
    today_btn_.signal_clicked().connect([this] { goToday(); });
    header_.pack_end(today_btn_, Gtk::PACK_SHRINK);  // left of evo_btn_
    vbox_.pack_start(cal_, Gtk::PACK_SHRINK);
    vbox_.pack_start(header_, Gtk::PACK_SHRINK);
    vbox_.pack_start(scroll_, Gtk::PACK_EXPAND_WIDGET);
    win_.add(vbox_);
    vbox_.show_all();
    // the widest day depends on the font, which the css sets
    day_label_.signal_style_updated().connect([this] { fitDayLabel(); });
    configure(config);

    cal_.signal_month_changed().connect([this] {
        request(shownKey());
        refresh();
    });
    cal_.signal_day_selected().connect([this] { refresh(); });
    evo_btn_.signal_clicked().connect([this] { launch({"evolution", "-c", "calendar"}); });
    win_.signal_key_press_event().connect([this](GdkEventKey *ev) {
        if (ev->keyval != GDK_KEY_Escape) return false;
        hide();
        return true;
    }, false);

    // warm up: connect to EDS and fetch this month while nobody is looking
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    request(month_key(t.tm_year + 1900, t.tm_mon));
}

Popup::~Popup() {
    Gtk::StyleContext::remove_provider_for_screen(win_.get_screen(), css_);
}

// Also on reload: a provider whose data is replaced restyles everything it
// applies to.
void Popup::configure(const awl_tray_config_t &config) {
    const auto &c = config.calendar;
    try {
        css_->load_from_data(make_css(config));
    } catch (const Glib::Error &e) {
        g_warning("awl calendar: css: %s", e.what().c_str());
    }
    vbox_.set_border_width(std::max(c.padding, 0));
    // GTK refuses a minimum above the maximum, also in passing
    int min = std::max(c.list_min_height, 0);
    int max = c.list_max_height > 0 ? std::max(c.list_max_height, min) : -1;
    scroll_.set_min_content_height(-1);
    scroll_.set_max_content_height(max);
    scroll_.set_min_content_height(min);
    // a mapped window keeps its size when its content shrinks; this snaps it
    // back to the natural size
    win_.resize(1, 1);
}

// the label's width is the widest "weekday, day month" of a (leap) year, in
// the label's bold font
void Popup::fitDayLabel() {
    auto layout = day_label_.create_pango_layout("");
    int widest = 0;
    char buf[64];
    for (int d = 1; d <= 366; d++) {
        time_t t0 = day_start(2024, 0, d);
        struct tm t;
        localtime_r(&t0, &t);
        strftime(buf, sizeof(buf), "%A, %e %B", &t);
        layout->set_markup("<b>" + Glib::Markup::escape_text(buf) + "</b>");
        int w, h;
        layout->get_pixel_size(w, h);
        widest = std::max(widest, w);
    }
    day_label_.set_size_request(widest, -1);
}

int Popup::shownKey() {
    guint y, m, d;
    cal_.get_date(y, m, d);
    return month_key((int)y, (int)m);
}

// Selecting the day first keeps it valid in both months (31 doesn't exist in
// every month); select_month() emits month-changed -> request().
void Popup::select(int year, int month, int day) {
    guint y, m, d;
    cal_.get_date(y, m, d);
    if ((int)y != year || (int)m != month) {
        cal_.select_day(1);
        cal_.select_month(month, year);
    }
    cal_.select_day(day);
}

void Popup::stepDay(int n) {
    guint y, m, d;
    cal_.get_date(y, m, d);
    time_t day = day_start((int)y, (int)m, (int)d + n);  // mktime() carries over
    struct tm t;
    localtime_r(&day, &t);
    select(t.tm_year + 1900, t.tm_mon, t.tm_mday);
}

// Keeps the day of the month, or the month's last day if it has fewer.
void Popup::stepMonth(int n) {
    guint y, m, d;
    cal_.get_date(y, m, d);
    int key = month_key((int)y, (int)m) + n;
    time_t last = day_start(key / 12, key % 12 + 1, 0);  // day 0: the one before the 1st
    struct tm t;
    localtime_r(&last, &t);
    select(key / 12, key % 12, std::min((int)d, t.tm_mday));
}

// From the bar's clock (awl_tray_calendar_scroll()): down is later, as on the
// calendar itself.
void Popup::scroll(int amount) {
    if (!shown_ || !amount) return;
    stepMonth(amount > 0 ? 1 : -1);
}

void Popup::request(int key) {
#ifdef AWL_HAVE_ECAL
    if (!g_pool || inflight_.count(key)) return;
    inflight_.insert(key);
    g_thread_pool_push(g_pool, new Query{key}, nullptr);
#else
    (void)key;
#endif
}

#ifdef AWL_HAVE_ECAL
void Popup::onResult(int key, std::vector<Event> events) {
    inflight_.erase(key);
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        if (a.all_day != b.all_day) return a.all_day;
        return a.start < b.start;
    });
    cache_[key] = std::move(events);
    if (key == shownKey()) refresh();
}
#endif

void Popup::refresh() {
    guint y, m, d;
    cal_.get_date(y, m, d);
    int key = month_key((int)y, (int)m);

    cal_.clear_marks();
    // the rows are GtkListBoxRows GTK created around our boxes
    for (auto c : list_.get_children()) gtk_widget_destroy(c->gobj());

    time_t day0 = day_start((int)y, (int)m, (int)d), day1 = day_start((int)y, (int)m, (int)d + 1);
    char buf[64];
    struct tm t;
    localtime_r(&day0, &t);
    strftime(buf, sizeof(buf), "%A, %e %B", &t);
    day_label_.set_markup("<b>" + Glib::Markup::escape_text(buf) + "</b>");

    auto it = cache_.find(key);
    if (it == cache_.end()) return;  // still loading
    int nrows = 0;
    // mark every day of the month an event touches (end is exclusive)
    const std::vector<Event> &events = it->second;
    for (int md = 1; day_start((int)y, (int)m, md) < month_start(key + 1); ++md) {
        time_t d0 = day_start((int)y, (int)m, md), d1 = day_start((int)y, (int)m, md + 1);
        if (std::any_of(events.begin(), events.end(), [&](const Event &e) {
            return e.start < d1 && std::max(e.end, e.start + 1) > d0;
        }))
            cal_.mark_day(md);
    }
    for (const Event &e : events) {
        if (!(e.start < day1 && std::max(e.end, e.start + 1) > day0)) continue;

        std::string when;
        if (e.all_day) {
            when = "all day";
        } else {
            char a[16], b[16];
            localtime_r(&e.start, &t);
            strftime(a, sizeof(a), "%H:%M", &t);
            localtime_r(&e.end, &t);
            strftime(b, sizeof(b), "%H:%M", &t);
            when = std::string(a) + "–" + b;
        }
        auto *row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 6);
        auto *dot = Gtk::make_managed<Gtk::Label>();
        dot->set_markup("<span foreground='" + Glib::Markup::escape_text(e.color.empty() ? "#888" : e.color) +
                        "'>●</span>");
        auto *time_l = Gtk::make_managed<Gtk::Label>(when);
        time_l->set_xalign(0);
        time_l->set_width_chars(11);
        auto *sum_l = Gtk::make_managed<Gtk::Label>(e.summary);
        sum_l->set_xalign(0);
        sum_l->set_ellipsize(Pango::ELLIPSIZE_END);
        sum_l->set_max_width_chars(30);
        if (!e.location.empty()) row->set_tooltip_text(e.location);
        row->pack_start(*dot, Gtk::PACK_SHRINK);
        row->pack_start(*time_l, Gtk::PACK_SHRINK);
        row->pack_start(*sum_l, Gtk::PACK_EXPAND_WIDGET);
        list_.append(*row);
        // display only, no hover highlight
        list_.get_row_at_index(nrows++)->set_activatable(false);
    }
    if (!nrows) {
        auto *none = Gtk::make_managed<Gtk::Label>("No events");
        none->set_sensitive(false);
        list_.append(*none);
    }
    list_.show_all();
}

void Popup::toggle(const std::string &mon, bool top) {
    bool same = shown_ && mon == mon_;
    hide();
    if (same) return;
    show(mon, top);
}

// Unlike toggle(), leaves the popup alone if it is already shown on mon.
void Popup::show(const std::string &mon, bool top) {
    if (shown_ && mon == mon_) return;
    hide();

    mon_ = mon;
    // awl binds the surface to that monitor (createlayersurface()) and
    // closes it on clicks elsewhere (buttonpress()); takes effect on the next
    // map. No margin needed: awl lays out non-exclusive layer surfaces in the
    // area next to the bar already.
    gtk_layer_set_namespace(win_.gobj(), ("awl-calendar:" + mon).c_str());
    gtk_layer_set_anchor(win_.gobj(), GTK_LAYER_SHELL_EDGE_TOP, top);
    gtk_layer_set_anchor(win_.gobj(), GTK_LAYER_SHELL_EDGE_BOTTOM, !top);

    goToday();  // always open on today
    win_.show();
    shown_ = true;
}

// Cached events show right away, the refetch replaces them when it comes
// back.
void Popup::goToday() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    cal_.select_month(t.tm_mon, t.tm_year + 1900);  // emits month-changed -> request()
    cal_.select_day(t.tm_mday);
    request(month_key(t.tm_year + 1900, t.tm_mon));
    refresh();
}

void Popup::hide() {
    if (!shown_) return;
    win_.hide();
    shown_ = false;
}

// Runs in the forked child: awl blocks the signals it handles in every
// thread and ignores SIGPIPE, and exec() would keep both.
void unblock_signals() {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    signal(SIGPIPE, SIG_DFL);
}

void Popup::launch(std::vector<std::string> argv) {
    try {
        // DO_NOT_REAP_CHILD: a direct child that awl's SIGCHLD handler
        // reaps, instead of GLib's double fork, whose waitpid() would race
        // with that handler.
        Glib::spawn_async("", argv, Glib::SPAWN_SEARCH_PATH | Glib::SPAWN_DO_NOT_REAP_CHILD,
                sigc::ptr_fun(&unblock_signals));
    } catch (const Glib::Error &e) {
        g_warning("awl calendar: cannot start %s: %s", argv[0].c_str(), e.what().c_str());
    }
    hide();
}

#ifdef AWL_HAVE_ECAL
gboolean deliver(gpointer data) {
    std::unique_ptr<Result> r(static_cast<Result *>(data));
    if (g_popup) g_popup->onResult(r->key, std::move(r->events));
    return G_SOURCE_REMOVE;
}
#endif

}  // namespace

// 0xRRGGBBAA as CSS. Not printf's %f: GTK sets the user's locale, whose
// decimal separator may be a comma.
std::string css_color(uint32_t c) {
    unsigned a = ((c & 0xff) * 1000 + 127) / 255;
    char buf[48];
    snprintf(buf, sizeof(buf), "rgba(%u, %u, %u, %u.%03u)", c >> 24, c >> 16 & 0xff,
             c >> 8 & 0xff, a / 1000, a % 1000);
    return buf;
}

void calendar_init(const awl_tray_config_t &config) {
    if (g_popup) return;
#ifdef AWL_HAVE_ECAL
    g_cancel = g_cancellable_new();
    g_pool = g_thread_pool_new(worker, nullptr, 1, FALSE, nullptr);
#endif
    g_popup = new Popup(config);
}

void calendar_fini() {
#ifdef AWL_HAVE_ECAL
    if (g_pool) {
        g_cancellable_cancel(g_cancel);
        // drops queued queries, waits for the running one
        g_thread_pool_free(g_pool, TRUE, TRUE);
        g_pool = nullptr;
    }
    for (auto &kv : g_clients) drop_client(kv.second);
    g_clients.clear();
    g_clear_object(&g_registry);
    g_clear_object(&g_cancel);
#endif
    // results still queued as idles find g_popup == nullptr and are dropped
    delete g_popup;
    g_popup = nullptr;
}

void calendar_configure(const awl_tray_config_t &config) {
    if (g_popup) g_popup->configure(config);
}

void calendar_toggle(const std::string &monitor_id, bool bar_on_top) {
    if (g_popup) g_popup->toggle(monitor_id, bar_on_top);
}

void calendar_show(const std::string &monitor_id, bool bar_on_top) {
    if (g_popup) g_popup->show(monitor_id, bar_on_top);
}

void calendar_hide() {
    if (g_popup) g_popup->hide();
}

void calendar_scroll(int amount) {
    if (g_popup) g_popup->scroll(amount);
}

}  // namespace awl
