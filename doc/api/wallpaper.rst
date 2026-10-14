The wallpaper
=============

The library's thread picks and decodes the next wallpaper
(plugins/wallpaper.c), awl takes the decoded image on the main thread and
shows it, fading from the previous one (awl_background.c). The modes the
``wallpaper`` and ``wallpapermode`` actions take are `WallpaperMode`, the
settings `WallpaperConfig`; both are in :doc:`awl.h <awl>`.

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
