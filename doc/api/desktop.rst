The desktop panel
=================

The list of the files in ``desktop_dir`` (a config dictionary key,
``$HOME/Desktop`` by default) over the wallpaper. awl places the
panels and owns their buffers (awl_desktop.c), the library scans the directory
and draws the list (plugins_desktop_panel.c).

awl_desktop.h
-------------

.. c:autodoc:: awl_desktop.h

awl_desktop.c
^^^^^^^^^^^^^

.. c:autodoc:: awl_desktop.c

plugins_desktop_panel.c
-----------------------

.. c:autodoc:: plugins_desktop_panel.c

plugins/readdir.h
-----------------

.. c:autodoc:: plugins/readdir.h
