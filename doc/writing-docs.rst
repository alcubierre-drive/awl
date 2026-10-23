Writing and building these docs
===============================

Building
--------

The documentation is built with Sphinx, the Read the Docs theme and
`Hawkmoth <https://jnikula.github.io/hawkmoth/>`_, which reads the C sources
with libclang. Once, set up the Python environment (a venv outside the
source tree, ``~/.local/share/venvs/awl-doc`` unless ``VENV`` says
otherwise)::

   make -C doc venv

Then, after every change::

   make -C doc

and open ``doc/_build/html/index.html``. The sources are parsed with the
flags meson.build uses (pkg-config, scenefx, ``-DXWAYLAND``) and the
generated protocol headers in ``build/``, so build there first:
``meson setup build && ninja -C build``.

Doc comments
------------

Hawkmoth documents what has a doc comment, ``/** ... */``, right before it.
Plain ``/* ... */`` comments stay out of the docs, so implementation notes
can stay plain.

* A doc comment at the top of a file, before the ``#include`` lines, is that
  file's overview.
* Struct members need their comment on the line(s) before them; a comment
  after a member on the same line isn't picked up.
* The text is reStructuredText. What is documented here is named with the C
  domain's roles, which link to it::

     /** like :c:func:`awl_draw_text`, through :c:member:`awl_host_t.actions`
      * (:c:type:`awl_host_t`), with :c:macro:`MODKEY`, :c:enum:`WallpaperMode`,
      * :c:enumerator:`AWL_NOTIF_OSD`, :c:var:`selmon` */

  ``:c:func:`` adds the parentheses; ``:c:type:`` is for typedefs and structs
  alike. ``~`` shows only the last part (``:c:member:`~widget_t.free```
  shows ``free``), ``title <target>`` another title: a member without a
  documentation of its own links to its struct, as in
  ``:c:member:`awl_config_t.tray```. Use double backquotes for
  code that isn't C or isn't documented here (````$HOME````,
  ````notify-send````), and for anything with a ``*`` in it, which
  reStructuredText would take for emphasis.
* ``make -C doc`` warns about every reference that doesn't resolve
  (``nitpicky`` in conf.py); a type used in a declaration needs a doc
  comment of its own, an enumerator too.
* Prose that doesn't belong to a single file goes into the ``.rst`` pages
  under ``doc/``.
