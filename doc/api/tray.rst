The tray
========

tray/awl_tray.h
---------------

.. c:autodoc:: tray/awl_tray.h

The items' menus
----------------

An item's menu (com.canonical.dbusmenu) is one of awl's own popups
(awl_window.h), not a GTK menu: the tray thread fetches it and reads its
icons' files, awl's main thread draws it and decodes the icons at the
monitor's scale. Each submenu is a popup of its own, beside the row it
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
