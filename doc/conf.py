# Sphinx configuration for awl's (awl's) documentation; see doc/Makefile.

import logging
import os
import re
import shlex
import subprocess
import sys

from hawkmoth import docstring as _hm_docstring

project = 'awl'
author = 'alcubierre-drive'
copyright = '2026, ' + author

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '_ext'))

extensions = ['hawkmoth', 'hawkmoth_inline', 'sphinxcontrib.mermaid']
# (hawkmoth_inline: members declared with an inline struct, see there)
# (sphinxcontrib.mermaid: the diagrams are drawn in the browser, by
# mermaid.js from its CDN, so viewing them needs a network connection)

# The C sources are parsed with libclang (the libclang wheel), with the flags
# meson.build builds with, so the parser sees what the compiler sees.
hawkmoth_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

_pkgs = ['wlroots-0.20', 'wayland-server', 'libpulse', 'xkbcommon', 'libinput',
         'pixman-1', 'fcft', 'libsystemd', 'xcb', 'xcb-icccm']
_pkg_cflags = subprocess.run(['pkg-config', '--cflags', *_pkgs], capture_output=True,
                             text=True, check=True).stdout

hawkmoth_clang = [
    '-std=c11',
    '-I' + hawkmoth_root,
    # the generated headers (protocols, version.h): meson setup build
    '-I' + os.path.join(hawkmoth_root, 'build'),
    '-DWLR_USE_UNSTABLE', '-D_POSIX_C_SOURCE=200809L', '-DXWAYLAND',
    # CUSTOM: where this machine's scenefx is installed (meson.options' awl_libs)
    '-I' + os.path.expanduser('~/Desktop/awl-libs/include/scenefx-0.5'),
    # every header is also parsed on its own
    '-Wno-pragma-once-outside-header',
    # libclang can't take gcc's x86 intrinsics headers, which Wuffs pulls in
    '-DWUFFS_CONFIG__AVOID_CPU_ARCH',
    *shlex.split(_pkg_cflags),
]
# The wheel's libclang comes without its own system headers (stddef.h & co),
# so take the include paths from the compiler meson.build uses.
hawkmoth_compiler = 'gcc'

# Doc comments are reStructuredText, naming what is documented with the C
# domain's roles, e.g. :c:func:`awl_redraw_request` or
# :c:member:`awl_host_t.actions` (see writing-docs.rst). A bare `name` is a C
# expression whose names link to their documentation.
primary_domain = 'c'
default_role = 'c:expr'

# Sphinx's C parser doesn't know _Atomic; as an attribute it renders as written
c_id_attributes = ['_Atomic']

# every reference must resolve: a broken one is a warning
nitpicky = True

# What isn't documented here, the
# C library's and the other libraries' types in the declarations, is no
# missing reference
nitpick_ignore_regex = [
    ('c:identifier', r'u?int(8|16|32|64)_t|size_t|pid_t|time_t|pthread_\w+|va_list|FILE|timeval|atomic_\w+'),
    ('c:identifier', r'(wlr|wl|pixman|fcft|sd|pa|libinput|wuffs_base)_\w+'),
    # an inline struct's type (hawkmoth_inline), shown as [anonymous]
    ('c:identifier', r'@\w+'),
]

html_theme = 'sphinx_rtd_theme'
html_static_path = ['_static']
html_css_files = ['custom.css']
html_theme_options = {
    'navigation_depth': 3,
    'collapse_navigation': False,
}

exclude_patterns = ['_build']


class _FileCommentFilter(logging.Filter):
    """A file's leading doc comment followed by an #include is that file's
    overview, which is just what hawkmoth makes of it, so drop its warning."""
    def filter(self, record):
        msg = record.getMessage()
        return not ('documentation comment attached to unexpected cursor' in msg
                    and 'INCLUSION_DIRECTIVE' in msg)


def _strip_last_line_prefix(app, lines, transform, options):
    """Hawkmoth strips the " * " prefix from a comment's inner lines only. The
    code closes its comments on the last text line ("* last words */"), so
    strip that one's too."""
    if len(lines) > 1:
        lines[-1] = re.sub(r'^[ \t]*\*[ \t]?', '', lines[-1])


_get_docstring = _hm_docstring.Docstring.get_docstring


def _fix_declaration(line):
    # clang spells `_Atomic float x` as `_Atomic(float) x`, which Sphinx's C
    # parser can't read
    line = re.sub(r'_Atomic\(([^()]*)\)', r'_Atomic \1', line)
    # hawkmoth puts an array parameter's size before its name:
    # `const char *[static n] fonts`
    line = re.sub(r'\*\[([^\]]*)\] (\w+)', r'*\2[\1]', line)
    return line


def _get_docstring_fixed(self, processor):
    """Declarations as Sphinx's C parser can read them, see _fix_declaration."""
    lines, line = _get_docstring(self, processor)
    lines = [_fix_declaration(l) if '.. c:' in l else l for l in lines]
    return lines, line


_hm_docstring.Docstring.get_docstring = _get_docstring_fixed


def setup(app):
    # first, before Sphinx's own filters count the warning
    for handler in logging.getLogger('sphinx').handlers:
        handler.filters.insert(0, _FileCommentFilter())
    app.connect('hawkmoth-process-docstring', _strip_last_line_prefix)
