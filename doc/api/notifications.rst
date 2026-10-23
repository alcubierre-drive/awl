Notifications
=============

awl itself is the notification server (awl_notifications.h): it owns
``org.freedesktop.Notifications`` on the session bus, on its main loop, so
the ids, the open notifications and the actions clients wait on outlive a
plugin reload. It keeps them as :c:type:`awl_notif_t` (awl_host.h) and tells the
plugin library when they change (:c:member:`~awl_plugin_api_t.notifications_changed` in
awl_plugin_abi.h); the library shows them and hands the user's clicks back
through the ``notif_*`` host calls. The body's markup is plugins/markup.h's.

awl's own notifications (the alerts, :c:func:`awl_notify` in the library) skip
D-Bus: :c:func:`awl_notifications_send`, the host call :c:member:`~awl_host_t.notif_send`, with an
:c:type:`awl_notif_send_t` that carries what D-Bus has no say in (its look,
:c:type:`awl_notif_look_t`). They come without a bus, and no client can replace or
close them. Their image may be a list of icon names, ``;`` between them, the
first found. One flagged :c:enumerator:`AWL_NOTIF_OSD` is feedback to what the user just
did (plugins/osd.h): transient, shown through do-not-disturb, and left out of
the menu and the bar widget's counts.

It also keeps a history of the last 100 closed ones (:c:type:`awl_notif_past_t`,
the host calls :c:member:`~awl_host_t.notif_history` and :c:member:`~awl_host_t.notif_forget`): the ones that
expired, were dismissed or made room, not the ones their client closed, an
action took or that were transient. It lives as long as awl, in memory only.
It keeps their contents, but not their actions and only up to 16 MiB of
image data, so that :c:member:`~awl_host_t.notif_show` can open one again: under a new id,
flagged :c:enumerator:`AWL_NOTIF_SHOWN`, which no client replaces, closes or hears of.

Timeouts count only while the user is there: after
:c:member:`awl_config_t.notifications.idle_pause` without a key or pointer motion
they stop, and the next one resumes them (:c:func:`awl_notifications_activity`).

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

The library's side: a persistent popup per notification (:c:member:`~awl_host_t.window_set_persistent`
in awl_host.h, see awl_popup.h), stacked from a corner as far as the window
area has room (:c:member:`~awl_host_t.window_room`): the last one cut to fit, "+k more" under it.

.. c:autodoc:: plugins/notify.h

plugins/notify_image.h
----------------------

The images named by files and icon names, decoded on a thread of their own.

.. c:autodoc:: plugins/notify_image.h

plugins/markup.h
----------------

The body's markup, parsed and laid out.

.. c:autodoc:: plugins/markup.h

plugins/notify_menu.h
---------------------

The menu of every open notification and its actions, with do-not-disturb and
"Dismiss all", then the history with its count and "Clear history", from the
bar's notification widget (:c:enumerator:`AwlWidgetNotifications`, a right click) and
from "+k more". It closes when the pointer leaves it and the widget. A past
one's "Show" button opens it again and closes the menu.

.. c:autodoc:: plugins/notify_menu.h

plugins/osd.h
-------------

The volume's and brightness's notification: an icon and a bar, no text,
from :c:func:`awl_volume_adjust`, the mutes and :c:func:`awl_brightness_adjust`.
See :doc:`/threads` for a change's way there and back.

.. c:autodoc:: plugins/osd.h

plugins/osd.c
^^^^^^^^^^^^^

.. c:autodoc:: plugins/osd.c
