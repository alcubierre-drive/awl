The launcher
============

The application launcher (MOD+p by default, the ``launcher`` action): one
of awl's popups that has the keyboard while it is shown (``window_set_key``
in `awl_host_t`), filled from the applications' .desktop files by a thread
of its own (see :doc:`/threads`).

It has two layouts (``awl_plugin_config_t.launcher.layout``):
``AWL_LAUNCHER_LIST``, the default, a box with the query above a list of
results, and ``AWL_LAUNCHER_BAR``, a one-line strip over the bar (the
``AWL_WINDOW_AT_BAR`` spot of awl_window.h) like awesome's menubar: the query
at the left, the results beside it in a row, each with its icon, paged so
the chosen one is always shown. Left and Right move along that row too.
``strip_at`` puts the strip over the bar (``AWL_WINDOW_AT_BAR``, the
default) or along the monitor's top or bottom edge
(``AWL_WINDOW_AT_STRIP_TOP``, ``AWL_WINDOW_AT_STRIP_BOTTOM``), wherever the
bar is. Results whose icon is missing take no room for one.

plugins/launcher.h
^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/launcher.h

plugins/fuzzy.h
^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/fuzzy.h

plugins/desktop_entry.h
^^^^^^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/desktop_entry.h
