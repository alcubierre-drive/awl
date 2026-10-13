#pragma once

#include <string>

#include "awl_tray.h"

// A small read-only popup (layer-shell window) next to the bar that shows a
// table of text for a bar widget, e.g. the IP widget's addresses: rows are
// separated by '\n', columns by '\t', and every cell is Pango markup. It
// looks like the calendar popup (config.calendar's colors and sizes).
//
// Everything here runs on the tray's GTK thread (awl_tray_bridge.cpp).

namespace awl {

void textpopup_init(const awl_tray_config_t& config);
void textpopup_fini();
// Applies changed settings (config.calendar): colors, sizes, the CSS.
void textpopup_configure(const awl_tray_config_t& config);
// Shows `text` on `monitor_id`, on the bar's side of the screen, centered
// on center_x (logical pixels from the monitor's left edge) but kept within
// the screen_width wide screen. With toggle, hides it instead if it is
// already shown there.
void textpopup_show(const std::string& monitor_id, bool bar_on_top, int center_x,
                    int screen_width, const std::string& text, bool toggle);
// Replaces the shown popup's text; nothing if it is hidden.
void textpopup_update(const std::string& text);
void textpopup_hide();

}  // namespace awl
