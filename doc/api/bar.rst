The bar
=======

Each monitor's bar is an `awl_draw_t` with three groups of `widget_t`: left, center
and right. awl draws the bar, lays the widgets out and routes pointer events
to them; the widgets themselves, and what they show, come from the plugin
library (plugins_widgets.c), so they reload with it.

awl_draw.h
----------

.. c:autodoc:: awl_draw.h

awl_redraw.h
------------

.. c:autodoc:: awl_redraw.h

plugins_widgets.c
-----------------

.. c:autodoc:: plugins_widgets.c
