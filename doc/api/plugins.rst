The plugins
===========

The data the bar shows, collected off the main thread (see
:doc:`/threads`). Each plugin keeps its output in atomics, which the widgets
read while drawing, and asks for a redraw when it changed (awl_redraw.h,
in :doc:`bar`).

Threads
-------

plugins/thread.h
^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/thread.h

plugins/poller.h
^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/poller.h

plugins/poller.c
^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/poller.c

plugins/logind.h
^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/logind.h

Persistent state
----------------

plugins/persistent.h
^^^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/persistent.h

Readings
--------

plugins/stats.h
^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/stats.h

plugins/temp.h
^^^^^^^^^^^^^^

.. c:autodoc:: plugins/temp.h

plugins/bat.h
^^^^^^^^^^^^^

.. c:autodoc:: plugins/bat.h

plugins/ipaddr.h
^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/ipaddr.h

plugins/date.h
^^^^^^^^^^^^^^

.. c:autodoc:: plugins/date.h

plugins/pulsetest.h
^^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/pulsetest.h

plugins/backlight.h
^^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/backlight.h

plugins/backlight.c
^^^^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/backlight.c

Colors
------

plugins/colors.h
^^^^^^^^^^^^^^^^

.. c:autodoc:: plugins/colors.h
