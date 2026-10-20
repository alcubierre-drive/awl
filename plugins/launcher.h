#pragma once

/**
 * The application launcher, in place of fuzzel: one of awl's popups
 * (awl_window.h) with the keyboard (``window_set_key``), shown by the
 * launcher action (plugins.h: `awl_launcher_toggle()`). It lists the
 * applications of the .desktop files in $XDG_DATA_HOME/applications and
 * each of $XDG_DATA_DIRS' (the Desktop Entry Specification: the first file
 * of an id wins, Hidden ones mask the id, NoDisplay, OnlyShowIn/NotShowIn
 * and TryExec heeded), matched with fuzzy.h against Name, GenericName,
 * Keywords and the command's name, ranked with how often and how recently
 * each was launched ($XDG_STATE_HOME/awl/launcher), which alone orders them
 * for an empty query.
 *
 * Two threads, like the desktop file list (plugins_desktop_panel.c): an
 * index thread reads the directories (again on inotify's word, so opening
 * is instant), the launch counts, and the icons the main thread asks for
 * (looked up and decoded at the size shown), since the main thread never
 * touches the file system. The window and the matching are the main
 * thread's.
 *
 * Two layouts (`awl_plugin_config_t.launcher`'s): a box with the query
 * above a list of results, or a strip over the bar with the results in a
 * row beside the query (like awesome's menubar), paged so that the chosen
 * one is shown.
 *
 * Keys: typing edits the query (BackSpace, Ctrl+W a word, Ctrl+U all of
 * it); Up/Down, Tab/Shift+Tab, Ctrl+P/Ctrl+N and Page Up/Down move, so do
 * Left/Right in the strip;
 * Return launches, Escape closes. Keys it doesn't take go to awl's
 * bindings, so the launcher's own closes it again. The pointer chooses,
 * clicks launch, the wheel scrolls; a press elsewhere closes it.
 *
 * The functions are in plugins.h; all of them main thread.
 */
