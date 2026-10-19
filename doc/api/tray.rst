The tray
========

tray/awl_tray.h
---------------

.. c:autodoc:: tray/awl_tray.h

The items' menus
----------------

An item's menu (com.canonical.dbusmenu) is one of awl's own popups
(awl_window.h), not a GTK menu: the tray thread fetches it and looks up and
reads its icons' files (plugins/icon.h), awl's main thread draws it and
decodes the icons at the monitor's scale. Each submenu is a popup of its own, beside the row it
belongs to; hovering opens and closes them after a short delay, and a new
layout that drops a submenu open (an app rebuilding its menu) waits a moment
for one that has it again. The menus look as config_plugins.h's
``popup_config.menu`` says (left out: like the other popups). ``test/sni_menu`` is an item with a menu of every
kind of entry, for trying it in test/live.sh's session.

A click on an item: the right button opens its menu, the left one activates
it (``Activate``), or opens its menu if it says it is one (``ItemIsMenu``)
or can't be activated. Resting the pointer on it shows its name
(tray/tooltip.h).

tray/menu.h
^^^^^^^^^^^

.. c:autodoc:: tray/menu.h

awl_post.h
^^^^^^^^^^

.. c:autodoc:: awl_post.h

tray/tooltip.h
^^^^^^^^^^^^^^

.. c:autodoc:: tray/tooltip.h

The bar's own tray
------------------

On trial, with config_awl.h's ``#define SNI_TRAY 1``: the tray as a bar
widget like the others, in C, without GTK, in place of the one above (which
then doesn't start). awl itself is the StatusNotifierWatcher (awl_sni.h), on
its main loop, so the items' registrations outlive a plugin reload; the
widget's host (plugins/sni.h) runs on the plugins' loop with sd-bus, and the
menus' D-Bus half is plugins/sni_menu.h. menu_window.c and tooltip.c are
built into the plugin library as well (with ``AWL_SNI_WIDGET``: windows of
its own, given back before it is unloaded). Clicks, tooltips and menus
behave as above.

awl_sni.h
^^^^^^^^^

.. c:autodoc:: awl_sni.h

plugins/sni.h
^^^^^^^^^^^^^

.. c:autodoc:: plugins/sni.h

plugins/sni_menu.h
^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/sni_menu.h

Icons
-----

Icon theme lookup and decoding without GTK (plugins/icon.h): the menus'
icons, and the bar's own tray's (plugins/sni.h). It is built into both
libraries. PNGs are decoded by Wuffs (wuffs/), SVGs by PlutoSVG
(plutosvg/svg.h), both vendored. An item's files and data are untrusted, so
anything unrealistically large is refused before it is decoded.

plugins/icon.h
^^^^^^^^^^^^^^

.. c:autodoc:: plugins/icon.h
