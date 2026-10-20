awl
===

The compositor itself. awl.h holds every type the plugin library shares with
awl, the configuration `awl_config_t` among them; awl_core.h the types only
awl's modules share; awl_server.h the core objects every part of awl uses
(the display, the backend, the scene, the seat). awl.c starts awl, runs its
main loop and takes it down again; everything else is a module of its own,
each with a header: the configuration, its defaults (awl_config_defaults.h) and its
reloads (awl_config.h), the
outputs (awl_output.h), the clients (awl_client.h, awl_xwayland.h), layouts
and tags (awl_layout.h), layer surfaces (awl_layer.h), the input devices and
the pointer (awl_input.h, awl_cursor.h), the screen lock (awl_lockscreen.h,
awl_alert.h), awl's own popups (awl_popup.h, awl_window.h), child
processes (awl_spawn.h) and the test hooks (awl_test.h). The bar
(:doc:`bar`) and the wallpaper (:doc:`wallpaper`) have pages of their own.

awl.h
-----

.. c:autodoc:: awl.h

awl_core.h
----------

.. c:autodoc:: awl_core.h

awl_server.h
------------

.. c:autodoc:: awl_server.h

awl.c
-----

.. c:autodoc:: awl.c

awl_config.h
------------

.. c:autodoc:: awl_config.h

awl_config_defaults.h
---------------------

.. c:autodoc:: awl_config_defaults.h

awl_output.h
------------

.. c:autodoc:: awl_output.h

awl_client.h
------------

.. c:autodoc:: awl_client.h

awl_xwayland.h
--------------

.. c:autodoc:: awl_xwayland.h

awl_layout.h
------------

.. c:autodoc:: awl_layout.h

awl_layer.h
-----------

.. c:autodoc:: awl_layer.h

awl_input.h
-----------

.. c:autodoc:: awl_input.h

awl_cursor.h
------------

.. c:autodoc:: awl_cursor.h

awl_lockscreen.h
----------------

.. c:autodoc:: awl_lockscreen.h

awl_alert.h
-----------

.. c:autodoc:: awl_alert.h

awl_popup.h
-----------

.. c:autodoc:: awl_popup.h

awl_window.h
------------

.. c:autodoc:: awl_window.h

awl_window_draw.h
-----------------

.. c:autodoc:: awl_window_draw.h

awl_spawn.h
-----------

.. c:autodoc:: awl_spawn.h

awl_log.h
---------

.. c:autodoc:: awl_log.h

awl_test.h
----------

.. c:autodoc:: awl_test.h
