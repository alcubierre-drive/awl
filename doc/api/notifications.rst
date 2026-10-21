Notifications
=============

awl itself is the notification server (awl_notifications.h): it owns
``org.freedesktop.Notifications`` on the session bus, on its main loop, so
the ids, the open notifications and the actions clients wait on outlive a
plugin reload. It keeps them as ``awl_notif_t`` (awl_host.h) and tells the
plugin library when they change (``notifications_changed`` in
awl_plugin_abi.h); the library shows them and hands the user's clicks back
through the ``notif_*`` host calls. The body's markup is plugins/markup.h's.

awl's own notifications (the alerts, ``awl_notify()`` in the library) skip
D-Bus: ``awl_notifications_send()``, the host call ``notif_send``, with an
``awl_notif_send_t`` that carries what D-Bus has no say in (its look,
``awl_notif_look_t``). They come without a bus, and no client can replace or
close them.

Its connection to the bus, and the tray watcher's, is awl_bus.h's: one each,
reconnected after a few seconds when lost.

awl_notifications.h
-------------------

.. c:autodoc:: awl_notifications.h

awl_bus.h
---------

.. c:autodoc:: awl_bus.h

plugins/notify.h
----------------

The library's side: a persistent popup per notification (``window_set_persistent``
in awl_host.h, see awl_popup.h), stacked from a corner as far as the window
area has room (``window_room``): the last one cut to fit, "+k more" under it.

.. c:autodoc:: plugins/notify.h
