#include "item.hpp"
#include "../awl_log.h"

#include <gdkmm/general.h>
#include <gtk-layer-shell.h>
#include <glibmm/main.h>
#include <gtkmm/tooltip.h>
#include <gtkmm/window.h>

#include <cstring>
#include <fstream>
#include <map>

/* #include "util/format.hpp" */
#include "gtk_icon.hpp"

namespace SNI {

static const Glib::ustring SNI_INTERFACE_NAME = sn_item_interface_info()->name;
static const unsigned UPDATE_DEBOUNCE_TIME = 10;

Item::Item(const std::string& bn, const std::string& op, Gtk::Window& win)
    : bus_name(bn),
      object_path(op),
      icon_size(16),
      effective_icon_size(0),
      icon_theme(Gtk::IconTheme::create()),
      win_(win) {
    icon_size = 16;
    scroll_threshold_ = 10.0;
    show_passive_ = true;

    win.signal_configure_event().connect_notify(sigc::mem_fun(*this, &Item::onConfigure));
    event_box.add(image);
    event_box.add_events(Gdk::BUTTON_PRESS_MASK | Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK);
    event_box.signal_button_press_event().connect(sigc::mem_fun(*this, &Item::handleClick));
    event_box.signal_scroll_event().connect(sigc::mem_fun(*this, &Item::handleScroll));
    // initial visibility
    event_box.show_all();
    event_box.set_visible(show_passive_);

    cancellable_ = Gio::Cancellable::create();

    auto interface = Glib::wrap(sn_item_interface_info(), true);
    Gio::DBus::Proxy::create_for_bus(Gio::DBus::BusType::BUS_TYPE_SESSION, bus_name, object_path,
                                     SNI_INTERFACE_NAME, sigc::mem_fun(*this, &Item::proxyReady),
                                     cancellable_, interface);
}

Item::~Item() { awl_tray_menu_free(menu_); }

void Item::onConfigure(GdkEventConfigure* ev) { (void)ev; this->updateImage(); }

void Item::proxyReady(Glib::RefPtr<Gio::AsyncResult>& result) {
    try {
        this->proxy_ = Gio::DBus::Proxy::create_for_bus_finish(result);
        /* Properties are already cached during object creation */
        auto cached_properties = this->proxy_->get_cached_property_names();
        for (const auto& name : cached_properties) {
            Glib::VariantBase value;
            this->proxy_->get_cached_property(value, name);
            setProperty(name, value);
        }

        this->proxy_->signal_signal().connect(sigc::mem_fun(*this, &Item::onSignal));

        if (this->id.empty() || this->category.empty()) {
            awl_err_printf("tray: invalid Status Notifier Item: %s, %s\n", bus_name.c_str(), object_path.c_str());
            return;
        }
        this->updateImage();

    } catch (const Glib::Error& err) {
        awl_wrn_printf("tray: failed to create DBus Proxy for %s %s: %s\n", bus_name.c_str(), object_path.c_str(), err.what().c_str());
    } catch (const std::exception& err) {
        awl_wrn_printf("tray: failed to create DBus Proxy for %s %s: %s\n", bus_name.c_str(), object_path.c_str(), err.what());
    }
}

template <typename T>
T get_variant(const Glib::VariantBase& value) {
    return Glib::VariantBase::cast_dynamic<Glib::Variant<T>>(value).get();
}

template <>
ToolTip get_variant<ToolTip>(const Glib::VariantBase& value) {
    ToolTip result;
    // Unwrap (sa(iiay)ss)
    auto container = value.cast_dynamic<Glib::VariantContainerBase>(value);
    result.icon_name = get_variant<Glib::ustring>(container.get_child(0));
    result.text = get_variant<Glib::ustring>(container.get_child(2));
    auto description = get_variant<Glib::ustring>(container.get_child(3));
    if (!description.empty()) {
        result.text = std::string("<b>") + result.text + std::string("</b>\n") + description;
        /* result.text = fmt::format("<b>{}</b>\n{}", result.text, description); */
    }
    return result;
}

void Item::setProperty(const Glib::ustring& name, Glib::VariantBase& value) {
    try {
        if (name == "Category") {
            category = get_variant<std::string>(value);
        } else if (name == "Id") {
            id = get_variant<std::string>(value);
        } else if (name == "Title") {
            title = get_variant<std::string>(value);
            if (tooltip.text.empty()) {
                event_box.set_tooltip_markup(title);
            }
        } else if (name == "Status") {
            setStatus(get_variant<Glib::ustring>(value));
        } else if (name == "IconName") {
            icon_name = get_variant<std::string>(value);
        } else if (name == "IconPixmap") {
            icon_pixmap = this->extractPixBuf(value.gobj());
        } else if (name == "OverlayIconName") {
            overlay_icon_name = get_variant<std::string>(value);
        } else if (name == "OverlayIconPixmap") {
            // TODO: overlay_icon_pixmap
        } else if (name == "AttentionIconName") {
            attention_icon_name = get_variant<std::string>(value);
        } else if (name == "AttentionIconPixmap") {
            // TODO: attention_icon_pixmap
        } else if (name == "AttentionMovieName") {
            attention_movie_name = get_variant<std::string>(value);
        } else if (name == "ToolTip") {
            tooltip = get_variant<ToolTip>(value);
            if (!tooltip.text.empty()) {
                event_box.set_tooltip_markup(tooltip.text);
            }
        } else if (name == "IconThemePath") {
            icon_theme_path = get_variant<std::string>(value);
            if (!icon_theme_path.empty()) {
                icon_theme->set_search_path({icon_theme_path});
            }
            if (menu_) awl_tray_menu_set_theme_path(menu_, icon_theme_path.c_str());
        } else if (name == "Menu") {
            auto path = get_variant<std::string>(value);
            if (path != menu) {
                awl_tray_menu_free(menu_);
                menu_ = nullptr;
                menu = path;
            }
        } else if (name == "ItemIsMenu") {
            item_is_menu = get_variant<bool>(value);
        }
    } catch (const Glib::Error& err) {
        awl_wrn_printf("tray: failed to set tray item property: %s.%s, value = %i, err = %s\n",
                id.empty() ? bus_name.c_str() : id.c_str(), name.c_str(), *reinterpret_cast<int*>(&value), err.what().c_str());
    } catch (const std::exception& err) {
        awl_wrn_printf("tray: failed to set tray item property: %s.%s, value = %i, err = %s\n",
                id.empty() ? bus_name.c_str() : id.c_str(), name.c_str(), *reinterpret_cast<int*>(&value), err.what());
    }
}

void Item::setStatus(const Glib::ustring& value) {
    Glib::ustring lower = value.lowercase();
    event_box.set_visible(show_passive_ || lower.compare("passive") != 0);

    auto style = event_box.get_style_context();
    for (const auto& class_name : style->list_classes()) {
        style->remove_class(class_name);
    }
    if (lower.compare("needsattention") == 0) {
        // convert status to dash-case for CSS
        lower = "needs-attention";
    }
    style->add_class(lower);
}

void Item::getUpdatedProperties() {
    auto params = Glib::VariantContainerBase::create_tuple(
        {Glib::Variant<Glib::ustring>::create(SNI_INTERFACE_NAME)});
    proxy_->call("org.freedesktop.DBus.Properties.GetAll",
                 sigc::mem_fun(*this, &Item::processUpdatedProperties), params);
};

void Item::processUpdatedProperties(Glib::RefPtr<Gio::AsyncResult>& _result) {
    try {
        auto result = proxy_->call_finish(_result);
        // extract "a{sv}" from VariantContainerBase
        Glib::Variant<std::map<Glib::ustring, Glib::VariantBase>> properties_variant;
        result.get_child(properties_variant);
        auto properties = properties_variant.get();

        for (const auto& [name, value] : properties) {
            if (update_pending_.count(name.raw())) {
                setProperty(name, const_cast<Glib::VariantBase&>(value));
            }
        }

        this->updateImage();
    } catch (const Glib::Error& err) {
        awl_wrn_printf("tray: failed to update properties: %s\n", err.what().c_str());
    } catch (const std::exception& err) {
        awl_wrn_printf("tray: failed to update properties: %s\n", err.what());
    }
    update_pending_.clear();
}

/**
 * Mapping from a signal name to a set of possibly changed properties.
 * Commented signals are not handled by the tray module at the moment.
 */
static const std::map<std::string_view, std::set<std::string_view>> signal2props = {
    {"NewTitle", {"Title"}},
    {"NewIcon", {"IconName", "IconPixmap"}},
    // {"NewAttentionIcon", {"AttentionIconName", "AttentionIconPixmap", "AttentionMovieName"}},
    // {"NewOverlayIcon", {"OverlayIconName", "OverlayIconPixmap"}},
    {"NewIconThemePath", {"IconThemePath"}},
    {"NewToolTip", {"ToolTip"}},
    {"NewStatus", {"Status"}},
    // {"XAyatanaNewLabel", {"XAyatanaLabel"}},
};

void Item::onSignal(const Glib::ustring& sender_name, const Glib::ustring& signal_name,
                    const Glib::VariantContainerBase& arguments) {
    (void)sender_name;
    (void)arguments;
    auto changed = signal2props.find(signal_name.raw());
    if (changed != signal2props.end()) {
        if (update_pending_.empty()) {
            /* Debounce signals and schedule update of all properties.
             * Based on behavior of Plasma dataengine for StatusNotifierItem.
             */
            Glib::signal_timeout().connect_once(sigc::mem_fun(*this, &Item::getUpdatedProperties),
                                                UPDATE_DEBOUNCE_TIME);
        }
        update_pending_.insert(changed->second.begin(), changed->second.end());
    }
}

static void pixbuf_data_deleter(const guint8* data) { g_free((void*)data); }

Glib::RefPtr<Gdk::Pixbuf> Item::extractPixBuf(GVariant* variant) {
    GVariantIter* it;
    g_variant_get(variant, "a(iiay)", &it);
    if (it == nullptr) {
        return Glib::RefPtr<Gdk::Pixbuf>{};
    }
    GVariant* val;
    gint lwidth = 0;
    gint lheight = 0;
    gint width;
    gint height;
    guchar* array = nullptr;
    while (g_variant_iter_loop(it, "(ii@ay)", &width, &height, &val)) {
        if (width > 0 && height > 0 && val != nullptr && width * height > lwidth * lheight) {
            auto size = g_variant_get_size(val);
            /* Sanity check */
            if (size == 4U * width * height) {
                /* Find the largest image */
                gconstpointer data = g_variant_get_data(val);
                if (data != nullptr) {
                    if (array != nullptr) {
                        g_free(array);
                    }
#if GLIB_MAJOR_VERSION >= 2 && GLIB_MINOR_VERSION >= 68
                    array = static_cast<guchar*>(g_memdup2(data, size));
#else
                    array = static_cast<guchar*>(g_memdup(data, size));
#endif
                    lwidth = width;
                    lheight = height;
                }
            }
        }
    }
    g_variant_iter_free(it);
    if (array != nullptr) {
        /* argb to rgba */
        for (uint32_t i = 0; i < 4U * lwidth * lheight; i += 4) {
            guchar alpha = array[i];
            array[i] = array[i + 1];
            array[i + 1] = array[i + 2];
            array[i + 2] = array[i + 3];
            array[i + 3] = alpha;
        }
        return Gdk::Pixbuf::create_from_data(array, Gdk::Colorspace::COLORSPACE_RGB, true, 8, lwidth,
                                             lheight, 4 * lwidth, &pixbuf_data_deleter);
    }
    return Glib::RefPtr<Gdk::Pixbuf>{};
}

void Item::updateImage() {
    try {
        // Query the scale factor exactly once and thread it through every call
        // below (instead of each of getScaledIconSize()/getIconByName()/here
        // separately calling image.get_scale_factor()) -- on a HiDPI window
        // those calls could otherwise observe different values (e.g. if the
        // widget gets realized in between two of them), sizing the pixbuf for
        // one scale but the Cairo surface for another. If that happens, the
        // surface can size out to logical 0 and never render anything.
        int scale_factor = std::max(1, image.get_scale_factor());
        auto pixbuf = getIconPixbuf(scale_factor);
        if (!pixbuf) {
            awl_err_printf("tray: item '%s': updateImage got a null pixbuf, leaving image unchanged\n", id.c_str());
            return;
        }
        int scaled_icon_size = icon_size * scale_factor;

        // If the loaded icon is not square, assume that the icon height should match the
        // requested icon size, but the width is allowed to be different. As such, if the
        // height of the image does not match the requested icon size, resize the icon such that
        // the aspect ratio is maintained, but the height matches the requested icon size.
        if (pixbuf->get_height() != scaled_icon_size) {
            int width = scaled_icon_size * pixbuf->get_width() / pixbuf->get_height();
            pixbuf = pixbuf->scale_simple(std::max(width, 1), scaled_icon_size,
                                          Gdk::InterpType::INTERP_BILINEAR);
        }

        // create_surface_from_pixbuf() needs a realized GdkWindow to size the
        // resulting HiDPI-aware surface correctly; without one (e.g. an item
        // registering before this Item's window has ever been shown/realized,
        // which is the common case for tray apps already running when the tray
        // starts up) it still returns a surface, but Gtk::Image ends up sizing
        // it wrong. Fall back to the plain pixbuf setter in that case.
        if (image.get_window()) {
            auto surface = Gdk::Cairo::create_surface_from_pixbuf(pixbuf, scale_factor, image.get_window());
            image.set(surface);
        } else {
            // A plain Gdk::Pixbuf carries no device-scale info -- Gtk::Image
            // treats its pixel dimensions as *logical* size directly. `pixbuf`
            // here is sized for HiDPI (scaled_icon_size = icon_size*scale_factor,
            // meant to back a Cairo surface above), so setting it as-is would
            // make this icon report scale_factor times its correct width
            // forever, reserving extra bar/tray space no visible icon fills.
            // (The original assumption was that onConfigure() re-running this
            // once the window is realized would swap in the correctly-sized
            // surface and self-correct -- confirmed via testing that this does
            // not reliably happen, so it can't be relied on to fix this up
            // later.) Downscale to the plain logical icon_size instead so the
            // reported size is right immediately; this just isn't HiDPI-crisp
            // until a later real icon update naturally replaces it.
            auto logical_pixbuf = pixbuf;
            if (scale_factor > 1) {
                int logical_h = std::max(1, scaled_icon_size / scale_factor);
                int logical_w = std::max(1, pixbuf->get_width() / scale_factor);
                logical_pixbuf = pixbuf->scale_simple(logical_w, logical_h, Gdk::InterpType::INTERP_BILINEAR);
            }
            image.set(logical_pixbuf);
        }
    } catch (const Glib::Error& err) {
        awl_err_printf("tray: item '%s': updateImage failed: %s\n", id.c_str(), err.what().c_str());
    } catch (const std::exception& err) {
        awl_err_printf("tray: item '%s': updateImage failed: %s\n", id.c_str(), err.what());
    }
}

Glib::RefPtr<Gdk::Pixbuf> Item::getIconPixbuf(int scale_factor) {
    int scaled_icon_size = icon_size * scale_factor;

    if (!icon_name.empty()) {
        try {
            std::ifstream temp(icon_name);
            if (temp.is_open()) {
                return Gdk::Pixbuf::create_from_file(icon_name);
            }
        } catch (Glib::Error& e) {
            // Ignore because we want to also try different methods of getting an icon.
            //
            // But a warning is logged, as the file apparently exists, but there was
            // a failure in creating a pixbuf out of it.

            awl_wrn_printf("tray: item '%s': %s\n", id.c_str(), e.what().c_str());
        }

        try {
            // Will throw if it can not find an icon.
            return getIconByName(icon_name, scaled_icon_size);
        } catch (Glib::Error& e) {
            awl_wrn_printf("tray: item '%s': lookup for icon '%s' at size %d failed: %s\n", id.c_str(),
                           icon_name.c_str(), scaled_icon_size, e.what().c_str());
        }
    }

    // Return the pixmap only if an icon for the given name could not be found.
    if (icon_pixmap) {
        return icon_pixmap;
    }

    if (icon_name.empty()) {
        awl_err_printf("tray: item '%s': no icon name or pixmap given\n", id.c_str());
    } else {
        awl_err_printf("tray: item '%s': could not find an icon named '%s' and no pixmap given\n",
                       id.c_str(), icon_name.c_str());
    }

    try {
        return getIconByName("image-missing", scaled_icon_size);
    } catch (Glib::Error& e) {
        // Even the built-in fallback icon failed to load (e.g. no icon theme
        // configured at all) -- synthesize a plain solid pixbuf so the item
        // still gets *something* visible/clickable instead of updateImage()
        // throwing and leaving image with no content set.
        awl_err_printf("tray: item '%s': fallback icon lookup failed too: %s\n", id.c_str(),
                       e.what().c_str());
        auto pixbuf = Gdk::Pixbuf::create(Gdk::Colorspace::COLORSPACE_RGB, true, 8, scaled_icon_size,
                                          scaled_icon_size);
        pixbuf->fill(0x808080ffu);
        return pixbuf;
    }
}

Glib::RefPtr<Gdk::Pixbuf> Item::getIconByName(const std::string& name, int request_size) {
    int tmp_size = 0;
    icon_theme->rescan_if_needed();
    auto sizes = icon_theme->get_icon_sizes(name.c_str());

    for (auto const& size : sizes) {
        // -1 == scalable
        if (size == request_size || size == -1) {
            tmp_size = request_size;
            break;
        } else if (size < request_size) {
            tmp_size = size;
        } else if (size > tmp_size && tmp_size > 0) {
            tmp_size = request_size;
            break;
        }
    }
    if (tmp_size == 0) {
        tmp_size = request_size;
    }
    if (!icon_theme_path.empty() &&
        icon_theme->lookup_icon(name.c_str(), tmp_size,
                                Gtk::IconLookupFlags::ICON_LOOKUP_FORCE_SIZE)) {
        return icon_theme->load_icon(name.c_str(), tmp_size,
                                     Gtk::IconLookupFlags::ICON_LOOKUP_FORCE_SIZE);
    }
    return DefaultGtkIconThemeWrapper::load_icon(name.c_str(), tmp_size,
                                                 Gtk::IconLookupFlags::ICON_LOOKUP_FORCE_SIZE);
}

// The menu, awl's (menu.h): on the monitor of the window, under the icon.
// The window's left margin is its x on the monitor (see Bridge::reposition()).
void Item::toggleMenu() {
    if (!menu_) menu_ = awl_tray_menu_new(bus_name.c_str(), menu.c_str(), icon_theme_path.c_str());
    int x = 0, y = 0;
    event_box.translate_coordinates(win_, event_box.get_allocated_width() / 2, 0, x, y);
    x += gtk_layer_get_margin(win_.gobj(), GTK_LAYER_SHELL_EDGE_LEFT);
    // "awl-tray:<monitor_id>", see Bridge::Bridge()
    const char* ns = gtk_layer_get_namespace(win_.gobj());
    const char* mon = ns && !strncmp(ns, "awl-tray:", 9) ? ns + 9 : "";
    awl_tray_menu_toggle(menu_, mon, x);
}

// TODO
bool Item::handleClick(GdkEventButton* const& ev) {
    // a double or triple click also comes as GDK_2BUTTON_PRESS (and
    // _3BUTTON_), after its presses: one action per press, or a menu
    // would open and close at once
    if (ev->type != GDK_BUTTON_PRESS) return true;
    auto parameters = Glib::VariantContainerBase::create_tuple(
        {Glib::Variant<int>::create(ev->x_root /*+ bar_.x_global */),
         Glib::Variant<int>::create(ev->y_root /*+ bar_.y_global */)});
    if ((ev->button == 1 && item_is_menu) || ev->button == 3) {
        if (!menu.empty()) {
            toggleMenu();
            return true;
        } else {
            proxy_->call("ContextMenu", parameters);
            return true;
        }
    } else if (ev->button == 1) {
        proxy_->call("Activate", parameters);
        return true;
    } else if (ev->button == 2) {
        proxy_->call("SecondaryActivate", parameters);
        return true;
    }
    return false;
}

bool Item::handleScroll(GdkEventScroll* const& ev) {
    int dx = 0, dy = 0;
    switch (ev->direction) {
        case GDK_SCROLL_UP:
            dy = -1;
            break;
        case GDK_SCROLL_DOWN:
            dy = 1;
            break;
        case GDK_SCROLL_LEFT:
            dx = -1;
            break;
        case GDK_SCROLL_RIGHT:
            dx = 1;
            break;
        case GDK_SCROLL_SMOOTH:
            distance_scrolled_x_ += ev->delta_x;
            distance_scrolled_y_ += ev->delta_y;
            // check against the configured threshold and ensure that the absolute value >= 1
            if (distance_scrolled_x_ > scroll_threshold_) {
                dx = (int)lround(std::max(distance_scrolled_x_, 1.0));
                distance_scrolled_x_ = 0;
            } else if (distance_scrolled_x_ < -scroll_threshold_) {
                dx = (int)lround(std::min(distance_scrolled_x_, -1.0));
                distance_scrolled_x_ = 0;
            }
            if (distance_scrolled_y_ > scroll_threshold_) {
                dy = (int)lround(std::max(distance_scrolled_y_, 1.0));
                distance_scrolled_y_ = 0;
            } else if (distance_scrolled_y_ < -scroll_threshold_) {
                dy = (int)lround(std::min(distance_scrolled_y_, -1.0));
                distance_scrolled_y_ = 0;
            }
            break;
    }
    if (dx != 0) {
        auto parameters = Glib::VariantContainerBase::create_tuple(
            {Glib::Variant<int>::create(dx), Glib::Variant<Glib::ustring>::create("horizontal")});
        proxy_->call("Scroll", parameters);
    }
    if (dy != 0) {
        auto parameters = Glib::VariantContainerBase::create_tuple(
            {Glib::Variant<int>::create(dy), Glib::Variant<Glib::ustring>::create("vertical")});
        proxy_->call("Scroll", parameters);
    }
    return true;
}

}  // namespace SNI
