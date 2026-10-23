The wallpaper
=============

The library's thread picks and decodes the next wallpaper
(plugins/wallpaper.c), awl takes the decoded image on the main thread and
shows it, fading from the previous one (awl_background.c). The modes the
:c:func:`wallpaper` and :c:func:`wallpapermode` actions take are :c:enum:`WallpaperMode`, the
settings :c:type:`awl_wallpaper_config_t`, the images :c:type:`awl_image_t`; all are in
:doc:`awl.h <awl>`. When the
wallpaper changes, and to which, is awl_wallpaper.c's.

awl_wallpaper.h
---------------

.. c:autodoc:: awl_wallpaper.h

awl_wallpaper.c
^^^^^^^^^^^^^^^

.. c:autodoc:: awl_wallpaper.c

awl_background.h
----------------

.. c:autodoc:: awl_background.h

awl_background.c
^^^^^^^^^^^^^^^^

.. c:autodoc:: awl_background.c

plugins/wallpaper.h
-------------------

.. c:autodoc:: plugins/wallpaper.h

plugins/wallpaper.c
^^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/wallpaper.c
