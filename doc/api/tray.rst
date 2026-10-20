The tray
========

The tray is a bar widget like the others, in C, without GTK. awl itself is
the StatusNotifierWatcher (awl_sni.h), on its main loop, so the items'
registrations outlive a plugin reload; the widget's host (plugins/sni.h)
runs on a loop of its own (plugins/loop.h) with sd-bus, and the menus' D-Bus half is
plugins/sni_menu.h. The widget's menus and tooltips are popups of the plugin
library's own, given back before it is unloaded.

A click on an item: the right button opens its menu, the left one activates
it (``Activate``), or opens its menu if it says it is one (``ItemIsMenu``)
or can't be activated. Resting the pointer on it shows its name
(plugins/tray_tooltip.h).

awl_sni.h
---------

.. c:autodoc:: awl_sni.h

plugins/sni.h
-------------

.. c:autodoc:: plugins/sni.h

The items' menus
----------------

An item's menu (com.canonical.dbusmenu) is one of awl's own popups
(awl_window.h): the tray's loop fetches it and looks up and reads its icons'
files (icons/icon.h), awl's main thread draws it and decodes the icons at
the monitor's scale. Each submenu is a popup of its own, beside the row it
belongs to; hovering opens and closes them after a short delay, and a new
layout that drops a submenu open (an app rebuilding its menu) waits a moment
for one that has it again. The menus look as ``c->popup.menu``
says (left out: like the other popups).
``test/sni_menu`` is an item with a menu of every kind of entry, for trying
it in test/live.sh's session.

plugins/sni_menu.h
^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/sni_menu.h

plugins/tray_menu.h
^^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/tray_menu.h

plugins/tray_tooltip.h
^^^^^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/tray_tooltip.h

Settings and the programs' icon theme
-------------------------------------

`awl_config_t.tray` (awl.h), and what awl's core keeps of the tray:
the icon theme of the programs started with awl.h's ``TRAY_ICONS``
(tray/app_env.c, and ``libawltray-env.so``, built next to awl from
tray/env_shim.c and preloaded into them).

.. c:autodoc:: tray/awl_tray.h

Icons
-----

Icon theme lookup and decoding without GTK (icons/icon.h), for the items'
icons and their menus'. PNGs are decoded by Wuffs (vendor/wuffs/), SVGs by
PlutoSVG (vendor/plutosvg/svg.h). An item's files and data are untrusted, so
anything unrealistically large is refused before it is decoded.

icons/icon.h
^^^^^^^^^^^^

.. c:autodoc:: icons/icon.h
