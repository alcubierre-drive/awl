Notifications
=============

awl itself is the notification server (awl_notifications.h): it owns
``org.freedesktop.Notifications`` on the session bus, on its main loop, so
the ids, the open notifications and the actions clients wait on outlive a
plugin reload. It keeps them as ``awl_notif_t`` (awl_host.h) and tells the
plugin library when they change (``notifications_changed`` in
awl_plugin_abi.h); the library shows them and hands the user's clicks back
through the ``notif_*`` host calls. The body's markup is plugins/markup.h's.

Its connection to the bus, and the tray watcher's, is awl_bus.h's: one each,
reconnected after a few seconds when lost.

awl_notifications.h
-------------------

.. c:autodoc:: awl_notifications.h

awl_bus.h
---------

.. c:autodoc:: awl_bus.h
