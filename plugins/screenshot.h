#pragma once

/**
 * Screenshots, in place of grim and slurp: the screenshot action
 * (:c:func:`awl_screenshot`) either takes one right away (the focused
 * window, the selected monitor, all of them) or lets the pointer choose
 * (:c:enumerator:`AWL_SCREENSHOT_SELECT`), through awl's grab
 * (:c:member:`awl_host_t.grab_start`):
 *
 * - a left click takes the window under the pointer, which is marked while
 *   the pointer is over it;
 * - a left drag takes the rectangle dragged (a press that moves more than
 *   :c:member:`screenshot.drag_px <awl_plugin_config_t.screenshot.drag_px>`);
 * - a right click takes the monitor under the pointer, with Shift every
 *   monitor;
 * - Escape, any other button or the action again takes none.
 *
 * awl renders the box again (:c:member:`awl_host_t.capture`, without the
 * mark), at the highest scale of the monitors it covers; a thread of the
 * screenshots' own encodes it as a PNG (plugins/png.h) and saves it in
 * :c:member:`screenshot.dir <awl_plugin_config_t.screenshot.dir>`, then the
 * main thread puts it on the clipboard (``image/png``,
 * :c:member:`awl_host_t.clipboard_set`) and says so in a notification with
 * the image. Main thread, but the thread.
 */

#include "handles.h"

/** the screenshot action's :c:member:`arg->i <Arg.i>` */
enum awl_screenshot_mode {
    /** the pointer chooses: a window, a rectangle, a monitor or all */
    AWL_SCREENSHOT_SELECT,
    /** the focused window */
    AWL_SCREENSHOT_WINDOW,
    /** the selected monitor */
    AWL_SCREENSHOT_MONITOR,
    /** every monitor */
    AWL_SCREENSHOT_ALL,
};

/** the screenshots' state and their thread; p is the root, whose config it
 * reads; NULL if it can't start. Every function below takes NULL as a
 * no-op. */
awl_screenshot_t* awl_screenshot_start( awl_plugin_data_t* p );
/** ends a grab, stops the thread (a screenshot it hasn't saved yet is
 * lost) and gives it all back (the plugin library's fini); returns nonzero
 * if the thread is stuck (in a write to a dead mount), which leaves it and
 * what it uses allocated */
int awl_screenshot_stop( awl_screenshot_t* s );
/** the screenshot action: takes one as mode (:c:enum:`awl_screenshot_mode`)
 * says; while the pointer chooses, ends that instead */
void awl_screenshot( awl_screenshot_t* s, int mode );
/** :c:member:`awl_plugin_api_t.redraw`: what the thread has done goes to the
 * clipboard and into a notification */
void awl_screenshot_redraw( awl_screenshot_t* s );
