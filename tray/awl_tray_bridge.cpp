// Bridges awl's bar (pure C, wlroots render thread) to the SNI tray logic
// in tray.hpp/host.hpp/item.hpp (C++, glib/gtk thread). Owns a dedicated
// pthread running a GTK/GLib main loop that hosts a real, input-owning
// gtk-layer-shell overlay window containing SNI::Tray's box of icons.
//
// This is deliberately close to the tray's original standalone design
// (a real GtkWindow that GTK renders and Wayland delivers input to
// directly; the items' menus are awl's own popups, see menu.h). The two
// things that change from the original: it runs as a library thread with a lifecycle
// awl owns (awl_tray_init()/awl_tray_shutdown() from awl.c's run()/
// cleanup()) instead of being spawned as a separate process, and it's
// auto-positioned from awl's real bar geometry/layout (awl_tray_set_bar_geometry
// from updatebar(), awl_tray_set_widget_x() from the systray widget's own
// draw call) instead of hardcoded gtk-layer-shell margins.
//
// awl's render thread never touches GTK/D-Bus directly: it calls
// awl_tray_width()/_set_widget_x()/_set_bar_geometry(), which only touch a
// few atomics/a small mutex, or marshal work onto the GTK thread via
// g_idle_add() (documented thread-safe).

#include "awl_tray.h"
#include "../awl_log.h"
#include "tray.hpp"

#include <gtk-layer-shell.h>
#include <gtkmm.h>

#include <pthread.h>

#include <cstdio>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SNI {
namespace {

struct BarGeom {
    int32_t x = 0, y = 0;
    int32_t width = 0, height = 20;
    double scale = 1.0;
    bool known = false;  // awl sent it
};

// What awl's thread and a monitor's Bridge (GTK thread) exchange. awl's
// thread only ever touches this, never the Bridge: it is created together
// with the monitor's registry entry (see shared_for()), so nothing awl sends
// is lost while the Bridge is still being constructed (updatebar() only
// sends the geometry on real changes), and it stays valid for whoever still
// holds a reference after the monitor is removed.
struct Shared {
    std::mutex mtx;
    BarGeom geom;                         // guarded by mtx
    std::atomic<int32_t> widget_x{0};     // buffer-scaled px, from systray_draw's `x`
    std::atomic<int32_t> content_w{0};    // logical px, box_'s current allocated width
    std::atomic<bool> visible{true};      // awl's togglebar

    BarGeom getGeom() {
        std::lock_guard<std::mutex> lg(mtx);
        return geom;
    }
};

// The settings in effect. Written by awl_tray_init() before the GTK thread
// exists, then only on the GTK thread (awl_tray_reload()), which is the only
// one that reads it.
awl_tray_config_t g_config;

// Marshals `fn` onto the tray's GTK/GLib thread. Safe to call from any
// thread (g_idle_add is documented thread-safe).
template <typename F>
void run_on_gtk_thread(F &&fn) {
    auto *boxed = new std::function<void()>(std::forward<F>(fn));
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            auto *f = static_cast<std::function<void()> *>(data);
            (*f)();
            delete f;
            return G_SOURCE_REMOVE;
        },
        boxed, nullptr);
}

// One monitor's tray window. Constructed, used and destroyed on the GTK
// thread only.
class Bridge {
public:
    Bridge(std::string monitor_id, std::shared_ptr<Shared> shared);
    ~Bridge();

    // Apply what awl last sent (shared_).
    void reposition();
    void applyVisible();
    void applyConfig();
    void reloadTray();

private:
    void scheduleWidthCheck();
    void checkWidth();
    void hookTray();

    std::string monitor_id_;
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<Gtk::Window> win_;
    std::unique_ptr<Tray> tray_;
    // Width tracking is event-driven (see scheduleWidthCheck()): GLib/GTK
    // sources and handlers registered with `this` as their data MUST be
    // removed in ~Bridge() before the Bridge itself goes away. Destroying a
    // Bridge doesn't implicitly cancel sources it registered, and a stale one
    // firing later dereferences a freed `this` -- the use-after-free that
    // used to crash intermittently around monitor disconnects/tray reloads
    // (back then via a recurring 200ms width-poll timer, now gone).
    //
    // Pending one-shot idle source for checkWidth(), 0 if none. Only touched
    // on the GTK thread.
    guint width_check_id_ = 0;
    // win_'s "check-resize": catch-all for any queued resize of the window's
    // content that the explicit Tray::on_change_ hooks don't cover.
    sigc::connection check_resize_conn_;

    int last_margin_left_ = INT32_MIN;
    int last_margin_top_ = INT32_MIN;
    int last_height_ = -1;
    int last_forced_w_ = -1;
    bool shown_ = false;
};

Bridge::Bridge(std::string monitor_id, std::shared_ptr<Shared> shared)
    : monitor_id_(std::move(monitor_id)), shared_(std::move(shared)) {
    win_ = std::make_unique<Gtk::Window>();
    win_->set_decorated(false);
    win_->set_name("awl-tray");

    gtk_layer_init_for_window(win_->gobj());
    // The namespace encodes which monitor this window belongs to as
    // "awl-tray:<monitor_id>" -- awl.c's createlayersurface() parses this
    // prefix and binds the surface to that exact wlr_output before falling
    // back to its normal "no output requested -> selmon" default. Without
    // this, every tray window (one per monitor, all requesting no specific
    // output) would land on whatever selmon happens to be, so only one
    // monitor would ever show a tray, and its window would fight between
    // monitors' geometry pushes (each monitor's own bar redraw calls
    // awl_tray_set_bar_geometry()/_set_widget_x() for its own Bridge now, so
    // that fight no longer exists once each is correctly bound to its own
    // output -- but binding is what actually prevents it).
    gtk_layer_set_namespace(win_->gobj(), ("awl-tray:" + monitor_id_).c_str());
    // BOTTOM, not TOP: awl's own bar lives in its LyrBottom scene layer (see
    // the comment on m->scene_buffer's creation in awl.c), which sits below
    // floating windows by design -- the tray should stay in sync with that,
    // not float above it, so a floating window dragged over the bar covers
    // both together instead of just the bar.
    gtk_layer_set_layer(win_->gobj(), GTK_LAYER_SHELL_LAYER_BOTTOM);
    gtk_layer_set_keyboard_interactivity(win_->gobj(), false);
    gtk_layer_set_anchor(win_->gobj(), GTK_LAYER_SHELL_EDGE_LEFT, true);
    gtk_layer_set_anchor(win_->gobj(), GTK_LAYER_SHELL_EDGE_TOP, true);
    // awl's own bar widget layout already reserves the horizontal space for
    // us (systray_draw's return value), so the layer surface itself must not
    // additionally reserve compositor exclusive zone space.
    gtk_layer_set_exclusive_zone(win_->gobj(), 0);

    applyConfig();

    tray_ = std::make_unique<Tray>("", *win_);
    // The original main.cpp called this right after constructing Tray;
    // Tray::update() is what actually makes box_ visible in the first place
    // (box_.set_visible(1)) -- without it box_ stays invisible and
    // contributes nothing to any preferred-size computation no matter what
    // icons it holds.
    tray_->update();
    // Tray/Item are unmodified original code with no opinion on vertical
    // alignment; win_ is a GtkBin so its single child (box_) stretch-fills
    // by default. Since our window is pinned to the bar's real height (which
    // can be taller than a single row of icons), center the row within it
    // instead of letting it stretch and (depending on how its children's own
    // internal allocation ends up sized) potentially clip/misplace icons.
    tray_->box_.set_valign(Gtk::ALIGN_CENTER);

    // GtkWindow only auto-sizes to its content's natural size up to the first
    // time it's actually resized (which happens as soon as the window is
    // first mapped, in reposition() below); after that it keeps whatever
    // size it was last given and won't grow/shrink on its own as icons come
    // and go. So whenever box_'s natural width may have changed, re-measure
    // it and force a renegotiation (checkWidth()). This is event-driven, with
    // no recurring timer:
    //  - Tray::on_change_ fires on item add/remove, item visibility (Status)
    //    changes and item image replacement (icon, icon_size, scale);
    //  - win_'s "check-resize" fires whenever GTK processes a queued resize
    //    of the (visible) window's content -- a catch-all for anything else
    //    that changes a child's size request (e.g. a style change);
    //  - reposition()/applyVisible() check when the window gets (re)mapped,
    //    and the constructor/reloadTray() schedule an initial check so the
    //    width is reported at startup even before any item exists.
    // All of these only *schedule* a single deduplicated idle source; the
    // actual measurement happens outside GTK's layout phase / widget
    // destruction, and is change-gated so it never feeds back into itself.
    hookTray();
    check_resize_conn_ = win_->signal_check_resize().connect([this] { scheduleWidthCheck(); });
    scheduleWidthCheck();

    // Deliberately not shown yet -- see reposition(): the window is first
    // mapped only once real bar geometry is known, so gtk-layer-shell's
    // initial size negotiation reflects the real bar height instead of
    // GTK's un-negotiated 200x200 fallback default.
}

Bridge::~Bridge() {
    // Everything that can call back into `this` must be cut off before the
    // Bridge goes away (see width_check_id_'s comment). All of it runs on this
    // same GTK thread, so nothing can fire concurrently with ~Bridge(), only
    // later. Order: first stop the change notifiers (destroying tray_/win_
    // can itself emit notify/check-resize while widgets are torn down), then
    // destroy them, then drop any idle source that was still pending or got
    // scheduled during that teardown.
    check_resize_conn_.disconnect();
    if (tray_) tray_->on_change_ = nullptr;
    tray_.reset();
    win_.reset();
    if (width_check_id_) {
        g_source_remove(width_check_id_);
        width_check_id_ = 0;
    }
}

void Bridge::hookTray() {
    // Tray outlives none of its notifiers (see Tray::on_change_), and Bridge
    // clears this before destroying tray_ (~Bridge()) or replaces the whole
    // Tray (reloadTray()), so capturing `this` here is safe.
    tray_->on_change_ = [this] { scheduleWidthCheck(); };
}

// Coalesces any burst of change notifications into one checkWidth() call on
// the next main-loop idle. GTK thread only.
void Bridge::scheduleWidthCheck() {
    if (width_check_id_) return;
    width_check_id_ = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        [](gpointer data) -> gboolean {
            auto *self = static_cast<Bridge *>(data);
            self->width_check_id_ = 0;
            self->checkWidth();
            return G_SOURCE_REMOVE;
        },
        this, nullptr);
}

void Bridge::checkWidth() {
    if (!win_ || !tray_) return;
    int min_w = 0, nat_w = 0;
    tray_->box_.get_preferred_width(min_w, nat_w);
    if (shared_->content_w.exchange(nat_w, std::memory_order_relaxed) != nat_w)
        awl_host->redraw_request();  // the bar reserves that width
    if (shown_ && nat_w != last_forced_w_) {
        last_forced_w_ = nat_w;
        int w = nat_w > 0 ? nat_w : 1;
        int h = last_height_ > 0 ? last_height_ : 1;
        // Deliberately resize to (nat_w, last_height_) rather than
        // gtk-layer-shell's documented "resize(1, 1) to snap back to natural
        // size" recipe: some icons' natural/preferred height (e.g. before their
        // image widget is realized) can exceed the bar's real height, and
        // resize(1, 1) lets GTK grow *both* axes to fit that, overflowing the
        // window past the bar strip. Pinning height explicitly keeps the window
        // clipped to the bar's actual height no matter what a child asks for.
        //
        // resize() alone is only a hint here and was observed to not actually
        // change this undecorated layer-shell window's real GTK allocation once
        // it had already been mapped with a smaller width (win_->get_allocation()
        // stayed stuck at width 1 forever). set_size_request() is a hard
        // constraint on the widget's size, not just a hint, and reliably forces
        // the reallocation that resize() alone didn't.
        //
        // This queues a resize, so "check-resize" fires once more and schedules
        // another checkWidth(), which then finds nat_w unchanged and stops.
        win_->set_size_request(w, h);
        win_->resize(w, h);
    }
}

void Bridge::reposition() {
    if (!win_) return;
    BarGeom g = shared_->getGeom();
    // the window is mapped only once the real geometry is known (see the
    // constructor); its arrival calls this again
    if (!g.known) return;
    int32_t wx = shared_->widget_x.load(std::memory_order_relaxed);

    // gtk-layer-shell margins get added by wlroots *directly* onto the
    // compositor's own output-layout coordinates (wlr_scene_layer_surface_v1_configure
    // adds margin straight onto full_area, i.e. m->m -- awl's own logical-pixel
    // space, physical / awl's fractional output scale) -- confirmed by
    // instrumenting awl.c's arrangelayer() directly and comparing its
    // resulting scene position against what was sent here. GTK's own
    // negotiated client-side scale (the legacy integer wl_output.scale
    // gtk-layer-shell sees) plays no part in that placement -- it only
    // matters for GTK's own rendering, e.g. converting box_'s preferred
    // width to physical pixels in width() below. So g.x/g.y/g.height (already
    // awl-logical, from updatebar() in awl.c) need no conversion at all here;
    // only wx -- physical/"buffer-scaled" per its documented contract, since
    // it comes from the bar's own pixman-buffer x coordinate -- needs
    // dividing by awl's own scale to land in that same awl-logical space.
    int margin_left = g.x + (int)(g.scale > 0 ? wx / g.scale : wx);
    int margin_top = g.y;
    int height = std::max(1, g.height);

    if (margin_left != last_margin_left_) {
        gtk_layer_set_margin(win_->gobj(), GTK_LAYER_SHELL_EDGE_LEFT, margin_left);
        last_margin_left_ = margin_left;
    }
    if (margin_top != last_margin_top_) {
        gtk_layer_set_margin(win_->gobj(), GTK_LAYER_SHELL_EDGE_TOP, margin_top);
        last_margin_top_ = margin_top;
    }
    if (height != last_height_) {
        // Use last_forced_w_ here too (not -1/unconstrained) -- set_size_request()
        // is what actually forces this window's real GTK allocation (see
        // checkWidth()), so resetting the width constraint to -1 on every height
        // change would silently undo whatever width checkWidth() had already
        // pinned, snapping the window back down to its unconstrained
        // (effectively 1px) width.
        int w = last_forced_w_ > 0 ? last_forced_w_ : 1;
        win_->set_size_request(w, height);
        last_height_ = height;
        // Re-pin height immediately (see checkWidth() for why an explicit
        // height is used instead of gtk-layer-shell's resize(1, 1) recipe).
        // Width tracking is left to checkWidth().
        if (shown_) win_->resize(w, height);
    }

    // First real geometry: map the window now, with the correct min-height
    // size request already in place (see comment in the constructor) -- but
    // only if the bar we're tracking isn't currently hidden (see applyVisible()).
    if (!shown_ && shared_->visible.load()) {
        win_->show();
        shown_ = true;
        // last_forced_w_ is still -1: pin the real content width now that the
        // window is mapped (checkWidth() only forces a size while shown_).
        scheduleWidthCheck();
    }
}

// GTK thread only.
void Bridge::applyVisible() {
    // Not mapped yet (e.g. toggled before the first real bar geometry ever
    // arrived) -- nothing to show/hide; reposition() checks visible itself
    // once it does map the window for the first time.
    if (!win_ || !shown_) return;
    if (shared_->visible.load()) {
        win_->show();
        scheduleWidthCheck();
    } else {
        win_->hide();
    }
}

// GTK thread only.
void Bridge::applyConfig() {
    if (!win_) return;
    uint32_t c = g_config.bg;
    Gdk::RGBA bg;
    bg.set_rgba_u((c >> 24 & 0xff) * 0x101, (c >> 16 & 0xff) * 0x101,
                  (c >> 8 & 0xff) * 0x101, (c & 0xff) * 0x101);
    win_->override_background_color(bg);
}

void Bridge::reloadTray() {
    if (!win_ || !tray_) return;
    // Keep the process-wide Watcher singleton (SNI::Watcher, which actually
    // owns the org.kde.StatusNotifierWatcher bus name every tray app on the
    // system registers its icon with) alive across the swap below,
    // independent of whichever Tray happens to hold the last strong
    // reference to it at any given instant. Without this, on a session with
    // only one monitor (one Bridge, one Tray), resetting tray_ below before
    // constructing its replacement would drop Watcher's refcount to zero and
    // momentarily tear down that bus-name ownership along with the one Host
    // actually being reloaded.
    auto keep_watcher_alive = Watcher::getInstance();

    // A GtkWindow (GtkBin) only ever holds one child -- explicitly detach the
    // old Tray's box_ before destroying it, so the new Tray's own win.add()
    // in its constructor below doesn't warn/fail against a still-attached
    // stale child. (Gtk::Bin::remove() takes no argument: a Bin only ever has
    // the one child anyway.)
    win_->remove();
    tray_.reset();
    tray_ = std::make_unique<Tray>("", *win_);
    hookTray();
    tray_->update();
    tray_->box_.set_valign(Gtk::ALIGN_CENTER);
    // The new Tray starts empty (items re-register asynchronously and each
    // fires on_change_), so report the emptied width right away too.
    scheduleWidthCheck();
}

// The tray's width for awl's bar. Any thread.
uint32_t tray_width(Shared &sh) {
    int32_t w = sh.content_w.load(std::memory_order_relaxed);
    if (w <= 0) return 0;
    // content_w is box_'s preferred width in *this window's own* GTK-logical
    // pixels; awl wants it back in units matching its own raw bar buffer (see
    // drawbar()'s m->b.width, which is m->b.real_width * m->wlr_output->scale)
    // to reserve bar space. That's awl's own *real* output scale (geom.scale,
    // e.g. 1.5) -- the same one reposition() uses for margins, per its comment
    // on wlr_scene_layer_surface_v1_configure(). It is NOT this window's own
    // client-negotiated buffer scale (win_->get_scale_factor()): GTK3/
    // gtk-layer-shell only understands the legacy integer wl_output.scale,
    // which wlroots reports as ceil(1.5)=2 for a 1.5x monitor -- using that
    // here over-reserved bar space by 2/1.5 (~33%), leaving a visible
    // transparent gap past the real icons' right edge.
    double scale = sh.getGeom().scale;
    if (scale <= 0) scale = 1.0;
    return (uint32_t)(w * scale + 0.5);
}

pthread_t g_thread;
std::atomic<bool> g_running{false};

// The monitors awl told us about, keyed by monitor_id (m->wlr_output->name).
// An entry is created by awl's thread (shared_for()) and removed by it
// (awl_tray_remove_monitor()), so a monitor that is unplugged and plugged
// back under the same name gets a fresh entry right away. Its Bridge is
// filled in later by the GTK thread, and is only ever constructed, used and
// destroyed there, outside the lock; the lock only guards the map itself.
struct Entry {
    std::shared_ptr<Shared> shared;
    std::unique_ptr<Bridge> bridge;  // null until the GTK thread built it
};
std::mutex g_bridges_mtx;
std::unordered_map<std::string, Entry> g_bridges;

// The Bridge of `mon`, or nullptr if there is none (yet). GTK thread only:
// the pointer stays valid until the current GTK callback returns, since only
// this thread destroys Bridges.
Bridge *gtk_bridge(const std::string &mon) {
    std::lock_guard<std::mutex> lg(g_bridges_mtx);
    auto it = g_bridges.find(mon);
    return it == g_bridges.end() ? nullptr : it->second.bridge.get();
}

// Destroys `b` on the GTK thread, where it was made.
void destroy_on_gtk_thread(std::unique_ptr<Bridge> b) {
    if (!b) return;
    Bridge *raw = b.release();
    run_on_gtk_thread([raw] { delete raw; });
}

// The shared state of `mon`, creating its entry and queueing the construction
// of its Bridge if it is new. nullptr if the tray isn't running. awl's thread.
std::shared_ptr<Shared> shared_for(const std::string &mon) {
    if (!g_running.load()) return nullptr;
    std::shared_ptr<Shared> sh;
    {
        std::lock_guard<std::mutex> lg(g_bridges_mtx);
        auto &e = g_bridges[mon];
        if (e.shared) return e.shared;
        sh = e.shared = std::make_shared<Shared>();
    }
    run_on_gtk_thread([mon, sh] {
        auto is_current = [&] {
            auto it = g_bridges.find(mon);
            return it != g_bridges.end() && it->second.shared == sh && !it->second.bridge;
        };
        {
            // the monitor may have gone away (or come back) meanwhile
            std::lock_guard<std::mutex> lg(g_bridges_mtx);
            if (!is_current()) return;
        }
        auto b = std::make_unique<Bridge>(mon, sh);
        Bridge *raw = b.get();
        {
            std::lock_guard<std::mutex> lg(g_bridges_mtx);
            if (is_current()) g_bridges[mon].bridge = std::move(b);
        }
        if (b) return;  // removed while it was being built; destroyed here
        // apply whatever awl sent before the Bridge existed
        raw->reposition();
    });
    return sh;
}

// Queues fn(bridge) on the GTK thread, if `mon` has a Bridge by then.
template <typename F>
void with_bridge(const std::string &mon, F &&fn) {
    run_on_gtk_thread([mon, fn = std::forward<F>(fn)] {
        if (Bridge *b = gtk_bridge(mon)) fn(*b);
    });
}

// Set by thread_main right before it returns, i.e. once Gtk::Main::run()
// has actually come back and all Bridges have been torn down -- see
// awl_tray_join()'s comment for why the caller polls this instead of just
// pthread_join()-ing directly.
std::atomic<bool> g_finished{false};

// Runs on its own thread. Gtk::Main's constructor connects to us as a
// Wayland client, which can only complete once awl's own main thread
// reaches wl_display_run() -- this blocks here (harmlessly, on this
// background thread) until then rather than the caller of awl_tray_init().
void *thread_main(void *) {
    int argc = 1;
    static char prog_name[] = "awltray";
    static char *argv0 = prog_name;
    char **argv = &argv0;
    Gtk::Main kit(argc, argv);

    // Bridges are created lazily, per monitor, the first time awl mentions a
    // monitor_id (see shared_for()) -- there's no single "the" tray window
    // to construct eagerly here any more.

    Gtk::Main::run();

    // Explicitly destroy every Bridge (and with it each one's win_/tray_,
    // i.e. this thread's own Wayland client state -- window/surface
    // destruction, final object cleanup) *before* signalling g_finished, so
    // the awl-side poll loop in awl_tray_join() can't observe "finished"
    // while that teardown is still in flight (which would let cleanup()
    // race ahead into wl_display_destroy_clients()/wl_display_destroy()
    // concurrently with this thread still using the display).
    std::unordered_map<std::string, Entry> bridges;
    {
        std::lock_guard<std::mutex> lg(g_bridges_mtx);
        bridges.swap(g_bridges);
    }
    bridges.clear();
    g_finished.store(true, std::memory_order_release);
    return nullptr;
}

// GLib's messages (GTK's, the D-Bus code's, our g_warning()s) as awl's;
// G_MESSAGES_DEBUG still decides about debug and info
GLogWriterOutput glib_writer(GLogLevelFlags flags, const GLogField *fields, gsize n, gpointer) {
    const char *domain = nullptr, *msg = "";
    int msglen = -1;
    for (gsize i = 0; i < n; ++i) {
        if (!strcmp(fields[i].key, "GLIB_DOMAIN"))
            domain = static_cast<const char *>(fields[i].value);
        else if (!strcmp(fields[i].key, "MESSAGE")) {
            msg = static_cast<const char *>(fields[i].value);
            msglen = static_cast<int>(fields[i].length);
        }
    }
    if (g_log_writer_default_would_drop(flags, domain)) return G_LOG_WRITER_HANDLED;
    awl_loglevel_t l = (flags & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL)) ? awl_loglevel_err
                     : (flags & G_LOG_LEVEL_WARNING) ? awl_loglevel_wrn
                     : (flags & G_LOG_LEVEL_DEBUG) ? awl_loglevel_vrb : awl_loglevel_log;
    if (msglen < 0) awl_dbg_printf(l, "%s: %s\n", domain ? domain : "GLib", msg);
    else awl_dbg_printf(l, "%s: %.*s\n", domain ? domain : "GLib", msglen, msg);
    return G_LOG_WRITER_HANDLED;
}

void route_logs() {
    static std::once_flag once;
    std::call_once(once, [] { g_log_set_writer_func(glib_writer, nullptr, nullptr); });
}

}  // namespace
}  // namespace SNI

extern "C" {

// awl_host.h; hidden, like the plugin library's copy
__attribute__((visibility("hidden"))) const awl_host_t *awl_host = nullptr;

void awl_tray_init(const awl_host_t *host, const awl_tray_config_t *config) {
    if (SNI::g_running.load()) return;
    // first: every log message goes through it
    awl_host = host;
    // the thread doesn't exist yet; pthread_create() publishes this to it
    SNI::g_config = *config;
    SNI::route_logs();
    // g_running before the thread exists: the thread's first calls into
    // awl (shared_for(), via geometry pushes) check it
    SNI::g_running = true;
    int err = pthread_create(&SNI::g_thread, nullptr, SNI::thread_main, nullptr);
    if (err) {
        awl_err_printf("can't start the tray thread: %s\n", strerror(err));
        SNI::g_running = false;
        return;
    }
    pthread_setname_np(SNI::g_thread, "awl-tray");
    awl_log_printf("tray thread started\n");
}

void awl_tray_shutdown(void) {
    if (!SNI::g_running.load()) return;
    // Deliberately does NOT pthread_join() here: by the time awl's cleanup()
    // calls this, wl_display_terminate() has already made wl_display_run()
    // return, so nothing is dispatching awl's own Wayland event loop any
    // more. If the tray's GTK thread needs a response from us as the server
    // to finish tearing down its Wayland client connection (a frame
    // callback, a layer-surface configure ack, a buffer release -- any of
    // which can happen during window/GTK teardown), a blocking join here
    // would deadlock forever waiting for an answer nobody is left to send.
    // See awl_tray_join(), which the caller must poll instead while still
    // pumping its own event loop.
    SNI::run_on_gtk_thread([] { Gtk::Main::quit(); });
}

int awl_tray_join(void) {
    if (!SNI::g_running.load()) return 1;
    if (!SNI::g_finished.load(std::memory_order_acquire)) return 0;
    pthread_join(SNI::g_thread, nullptr);
    SNI::g_running = false;
    return 1;
}

uint32_t awl_tray_width(const char *monitor_id) {
    auto sh = SNI::shared_for(monitor_id ? monitor_id : "");
    return sh ? SNI::tray_width(*sh) : 0;
}

void awl_tray_set_widget_x(const char *monitor_id, uint32_t x) {
    std::string mon(monitor_id ? monitor_id : "");
    auto sh = SNI::shared_for(mon);
    if (!sh) return;
    if (sh->widget_x.exchange((int32_t)x, std::memory_order_relaxed) != (int32_t)x)
        SNI::with_bridge(mon, [](SNI::Bridge &b) { b.reposition(); });
}

void awl_tray_set_bar_geometry(const char *monitor_id, int32_t x, int32_t y, uint32_t width, uint32_t height, double scale) {
    std::string mon(monitor_id ? monitor_id : "");
    auto sh = SNI::shared_for(mon);
    if (!sh) return;
    SNI::BarGeom g{x, y, (int32_t)width, (int32_t)height, scale, true};
    bool moved;
    {
        std::lock_guard<std::mutex> lg(sh->mtx);
        const SNI::BarGeom &o = sh->geom;
        moved = !o.known || o.x != g.x || o.y != g.y || o.width != g.width ||
                o.height != g.height || o.scale != g.scale;
        sh->geom = g;
    }
    if (moved) SNI::with_bridge(mon, [](SNI::Bridge &b) { b.reposition(); });
}

void awl_tray_reload(const awl_tray_config_t *config) {
    if (!SNI::g_running.load()) return;
    SNI::run_on_gtk_thread([config = *config] {
        SNI::g_config = config;
        // reloadTray() is GTK work; don't hold the lock for it
        std::vector<SNI::Bridge *> bridges;
        {
            std::lock_guard<std::mutex> lg(SNI::g_bridges_mtx);
            for (auto &kv : SNI::g_bridges)
                if (kv.second.bridge) bridges.push_back(kv.second.bridge.get());
        }
        for (SNI::Bridge *b : bridges) {
            b->applyConfig();
            b->reloadTray();
        }
    });
}

void awl_tray_set_visible(const char *monitor_id, int visible) {
    std::string mon(monitor_id ? monitor_id : "");
    auto sh = SNI::shared_for(mon);
    if (!sh) return;
    if (sh->visible.exchange(visible != 0) != (visible != 0))
        SNI::with_bridge(mon, [](SNI::Bridge &b) { b.applyVisible(); });
}

void awl_tray_remove_monitor(const char *monitor_id) {
    std::string mon(monitor_id ? monitor_id : "");
    std::unique_ptr<SNI::Bridge> b;
    {
        std::lock_guard<std::mutex> lg(SNI::g_bridges_mtx);
        auto it = SNI::g_bridges.find(mon);
        if (it == SNI::g_bridges.end()) return;
        b = std::move(it->second.bridge);
        SNI::g_bridges.erase(it);
    }
    SNI::destroy_on_gtk_thread(std::move(b));
}

}  // extern "C"
