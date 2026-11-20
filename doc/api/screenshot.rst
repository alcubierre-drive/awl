Screenshots
===========

The screenshot action (Print by default: the pointer chooses; Shift+Print:
the focused window) in place of grim and slurp. The plugin library asks awl
for a pointer grab and a mark (:c:member:`~awl_host_t.grab_start`,
:c:member:`~awl_host_t.grab_mark`), for what is where
(:c:member:`~awl_host_t.client_at`, :c:member:`~awl_host_t.monitor_box`, ...),
for the image (:c:member:`~awl_host_t.capture`, the scene rendered again for
the box) and to own the clipboard (:c:member:`~awl_host_t.clipboard_set`);
awl's side is awl_capture.h. A thread of the screenshots' own encodes and
saves the PNG (see :doc:`/threads`) with plugins/png.h, which deflates with
the vendored sdefl (vendor/sdefl/sdefl.h).

plugins/screenshot.h
^^^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/screenshot.h

plugins/screenshot.c
^^^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/screenshot.c

plugins/png.h
^^^^^^^^^^^^^

.. c:autodoc:: plugins/png.h

plugins/png.c
^^^^^^^^^^^^^

.. c:autodoc:: plugins/png.c
