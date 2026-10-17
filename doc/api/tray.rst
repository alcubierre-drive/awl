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
monitor's scale. ``test/sni_menu`` is an item with a menu of every kind of
entry, for trying it in test/live.sh's session.

tray/menu.h
^^^^^^^^^^^

.. c:autodoc:: tray/menu.h

awl_post.h
^^^^^^^^^^

.. c:autodoc:: awl_post.h
