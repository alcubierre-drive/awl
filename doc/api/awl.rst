awl
===

The compositor itself. awl.h holds the types the config files and the plugin library
share with awl.c; awl.c's internals below are the parts specific to this fork
(the plugin library, the bar's hover, the wallpaper timer, config reloads,
child processes).

awl.h
-----

.. c:autodoc:: awl.h

awl.c
-----

.. c:autodoc:: awl.c

awl_log.h
---------

.. c:autodoc:: awl_log.h

awl_lock.h
----------

.. c:autodoc:: awl_lock.h

awl_alert.h
-----------

.. c:autodoc:: awl_alert.h
