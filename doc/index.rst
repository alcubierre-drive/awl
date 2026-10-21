awl
===

awl is a Wayland compositor that grew out of `dwl
<https://codeberg.org/dwl/dwl>`_, the dwm-like compositor based on wlroots,
and has moved far enough from it to go by its own name. It extends dwl into a
small desktop: a bar with status widgets and popups (a calendar under the
clock), a system tray, a
wallpaper with timed changes and fades, a panel with the files on the
desktop, and notifications. Most of that is in a separate library that a
running awl can rebuild and reload without a restart. The bar's drawing code
started out as `drwl <https://codeberg.org/sewn/drwl>`_.

.. toctree::
   :maxdepth: 2
   :caption: Overview

   overview
   reload
   threads

.. toctree::
   :maxdepth: 2
   :caption: Reference

   api/plugin-abi
   api/bar
   api/plugins
   api/wallpaper
   api/desktop
   api/tray
   api/notifications
   api/launcher
   api/awl

.. toctree::
   :maxdepth: 1
   :caption: About these docs

   writing-docs

* :ref:`genindex`
