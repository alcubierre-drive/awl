The bar
=======

Each monitor's bar is an `awl_draw_t`, part of its `Monitor` (``drw``), with
its font and three groups of `widget_t`: left, center and right. awl draws
the bar, lays the widgets out and routes pointer events to them; the widgets
themselves, and what they show, come from the plugin library
(plugins_widgets.c), so they reload with it.

Text is drawn apart from the bar: `awl_draw_text()`, `awl_draw_text_over()`
and `awl_draw_text_width()` take any pixman image (`awl_draw_image()`) and a
font from the font registry (`awl_font_get()`). A widget draws with its
``pix`` and its bar's font, a popup's painter with the popups' font
(`Monitor.popupfont`, else the bar's), the desktop file list with its own
(`Monitor.desktopfont`, else the bar's).

awl_draw.h
----------

.. c:autodoc:: awl_draw.h

awl_redraw.h
------------

.. c:autodoc:: awl_redraw.h

plugins_widgets.c
-----------------

.. c:autodoc:: plugins_widgets.c
