The bar
=======

Each monitor's bar is an :c:type:`awl_bar_t`, part of its :c:type:`Monitor` (:c:member:`~Monitor.bar`), with
its font and three groups of :c:type:`widget_t`: left, center and right. awl draws
the bar, lays the widgets out and routes pointer events to them; the widgets
themselves, and what they show, come from the plugin library
(plugins_widgets.c), so they reload with it.

Text is drawn apart from the bar: :c:func:`awl_draw_text`, :c:func:`awl_draw_text_over`
and :c:func:`awl_draw_text_width` take any pixman image (:c:func:`awl_draw_image`) and a
font from the font registry (:c:func:`awl_font_get`). A widget draws with its
``pix`` and its bar's font, a popup's painter with the popups' font
(:c:member:`Monitor.popupfont`, else the bar's), the desktop file list with its own
(:c:member:`Monitor.desktopfont`, else the bar's).

awl_bar_type.h
--------------

.. c:autodoc:: awl_bar_type.h

awl_bar.h
---------

.. c:autodoc:: awl_bar.h

awl_draw.h
----------

.. c:autodoc:: awl_draw.h

awl_redraw.h
------------

.. c:autodoc:: awl_redraw.h

plugins_widgets.c
-----------------

.. c:autodoc:: plugins_widgets.c
