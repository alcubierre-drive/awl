#include "textpopup.hpp"
#include "calendar.hpp"

#include <gtk-layer-shell.h>
#include <gtkmm.h>

#include <algorithm>
#include <memory>

namespace awl {
namespace {

// Same look as the calendar (see make_css() there), scoped to #awl-popup.
std::string make_css(const awl_tray_config_t &config) {
    const auto &c = config.calendar;
    gchar *css = g_strdup_printf(R"css(
#awl-popup {
  background-color: %s;
  border: %dpx solid %s;
  border-radius: %dpx;
}
#awl-popup * {
  background-color: transparent;
  background-image: none;
  border-color: transparent;
  box-shadow: none;
  text-shadow: none;
  color: %s;
}
#awl-popup .dim { color: %s; }
)css",
        css_color(c.bg).c_str(), std::max(c.border_width, 0), css_color(c.border).c_str(),
        std::max(c.radius, 0), css_color(c.fg).c_str(), css_color(c.dim).c_str());
    std::string s = css;
    g_free(css);
    return s;
}

class Popup {
public:
    explicit Popup(const awl_tray_config_t &config);
    ~Popup();
    void configure(const awl_tray_config_t &config);
    void show(const std::string &mon, bool top, int center_x, int screen_width,
              const std::string &text, bool toggle);
    void update(const std::string &text);
    void hide();

private:
    void fill(const std::string &text);
    void place();

    Glib::RefPtr<Gtk::CssProvider> css_;
    Gtk::Window win_;
    Gtk::Grid grid_;
    std::string mon_, text_;
    bool shown_ = false;
    int center_x_ = 0, screen_width_ = 0, margin_ = -1;
};

Popup *g_popup = nullptr;

Popup::Popup(const awl_tray_config_t &config) {
    win_.set_decorated(false);
    win_.set_name("awl-popup");
    if (GdkVisual *visual = gdk_screen_get_rgba_visual(win_.get_screen()->gobj()))
        gtk_widget_set_visual(GTK_WIDGET(win_.gobj()), visual);
    css_ = Gtk::CssProvider::create();
    Gtk::StyleContext::add_provider_for_screen(win_.get_screen(), css_,
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_layer_init_for_window(win_.gobj());
    gtk_layer_set_layer(win_.gobj(), GTK_LAYER_SHELL_LAYER_TOP);
    // like the calendar: exclusive while shown, so Escape closes it
    gtk_layer_set_keyboard_mode(win_.gobj(), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_layer_set_exclusive_zone(win_.gobj(), 0);
    gtk_layer_set_anchor(win_.gobj(), GTK_LAYER_SHELL_EDGE_RIGHT, true);

    grid_.set_column_spacing(12);
    grid_.set_row_spacing(2);
    win_.add(grid_);
    grid_.show();
    configure(config);
    // the width is only final once GTK has allocated it
    win_.signal_size_allocate().connect([this](Gtk::Allocation &) { place(); });

    win_.signal_key_press_event().connect([this](GdkEventKey *ev) {
        if (ev->keyval != GDK_KEY_Escape) return false;
        hide();
        return true;
    }, false);
}

Popup::~Popup() {
    Gtk::StyleContext::remove_provider_for_screen(win_.get_screen(), css_);
}

void Popup::configure(const awl_tray_config_t &config) {
    try {
        css_->load_from_data(make_css(config));
    } catch (const Glib::Error &e) {
        g_warning("awl popup: css: %s", e.what().c_str());
    }
    grid_.set_border_width(std::max(config.calendar.padding, 0));
    win_.resize(1, 1);
}

void Popup::fill(const std::string &text) {
    text_ = text;
    for (auto c : grid_.get_children()) gtk_widget_destroy(c->gobj());
    size_t row = 0, pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        size_t col = 0, cpos = 0;
        while (cpos <= line.size()) {
            size_t tab = line.find('\t', cpos);
            if (tab == std::string::npos) tab = line.size();
            auto *l = Gtk::make_managed<Gtk::Label>();
            l->set_markup(line.substr(cpos, tab - cpos));
            l->set_xalign(0);
            grid_.attach(*l, (int)col++, (int)row);
            cpos = tab + 1;
        }
        row++;
        pos = eol + 1;
    }
    grid_.show_all();
    // a mapped window keeps its size when its content shrinks
    win_.resize(1, 1);
}

// Anchored to the right edge: the margin that centers the popup on
// center_x_, moved as far as needed to stay on the screen (at its left edge
// if it is wider than the screen).
void Popup::place() {
    int w = win_.get_allocated_width();
    if (!win_.get_mapped() || w <= 1) {
        int min, nat;
        win_.get_preferred_width(min, nat);
        w = nat;
    }
    int left = center_x_ - w / 2;
    left = std::max(0, std::min(left, screen_width_ - w));
    int margin = std::max(0, screen_width_ - left - w);
    if (margin == margin_) return;
    margin_ = margin;
    gtk_layer_set_margin(win_.gobj(), GTK_LAYER_SHELL_EDGE_RIGHT, margin);
}

void Popup::show(const std::string &mon, bool top, int center_x, int screen_width,
                 const std::string &text, bool toggle) {
    bool same = shown_ && mon == mon_;
    if (same && toggle) {
        hide();
        return;
    }
    if (text != text_) fill(text);
    center_x_ = center_x;
    screen_width_ = screen_width;
    place();
    if (same) return;
    hide();
    mon_ = mon;
    // awl binds the surface to that monitor and closes it on clicks
    // elsewhere, as for the calendar (awl.c); takes effect on the next map
    gtk_layer_set_namespace(win_.gobj(), ("awl-popup:" + mon).c_str());
    gtk_layer_set_anchor(win_.gobj(), GTK_LAYER_SHELL_EDGE_TOP, top);
    gtk_layer_set_anchor(win_.gobj(), GTK_LAYER_SHELL_EDGE_BOTTOM, !top);
    win_.show();
    shown_ = true;
}

void Popup::update(const std::string &text) {
    if (shown_ && text != text_) fill(text);
}

void Popup::hide() {
    if (!shown_) return;
    win_.hide();
    shown_ = false;
}

}  // namespace

void textpopup_init(const awl_tray_config_t &config) {
    if (!g_popup) g_popup = new Popup(config);
}

void textpopup_fini() {
    delete g_popup;
    g_popup = nullptr;
}

void textpopup_configure(const awl_tray_config_t &config) {
    if (g_popup) g_popup->configure(config);
}

void textpopup_show(const std::string &monitor_id, bool bar_on_top, int center_x,
                    int screen_width, const std::string &text, bool toggle) {
    if (g_popup) g_popup->show(monitor_id, bar_on_top, center_x, screen_width, text, toggle);
}

void textpopup_update(const std::string &text) {
    if (g_popup) g_popup->update(text);
}

void textpopup_hide() {
    if (g_popup) g_popup->hide();
}

}  // namespace awl
