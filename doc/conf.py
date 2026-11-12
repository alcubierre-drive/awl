# Sphinx configuration for awl's (awl's) documentation; see doc/Makefile.

import logging
import os
import re
import shlex
import subprocess
import sys

from docutils.statemachine import ViewList
import hawkmoth
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
    ('c:identifier', r'(wlr|wl|pixman|fcft|sd|pa|libinput|wuffs_base|xkb)_\w+'),
    # the modules' own state in awl_server_t, defined in .c files the docs
    # leave out
    ('c:identifier', r'awl_(bars|alerts|input|sni_watcher|notifs|test)'),
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


_DIRECTIVE = re.compile(r'^( *)\.\. c:(member|var):: ')


def _merge_declarators(viewlist):
    """`int x, y;` under one doc comment: hawkmoth makes a directive for each
    declarator, each with the whole comment. Make them one directive with a
    signature for each, under the comment once (any neighbours of the same
    kind with the same text, which only declarators sharing a comment have)."""
    lines = [(viewlist[i], viewlist.source(i), viewlist.offset(i)) for i in range(len(viewlist))]

    # split into blocks: a member's or variable's directive with its body, or
    # anything else line by line
    blocks, i = [], 0
    while i < len(lines):
        m = _DIRECTIVE.match(lines[i][0])
        if not m:
            blocks.append((None, [lines[i]], []))
            i += 1
            continue
        indent = len(m.group(1))
        j = i + 1
        while j < len(lines) and (not lines[j][0].strip()
                                  or len(lines[j][0]) - len(lines[j][0].lstrip()) > indent):
            j += 1
        body = lines[i + 1:j]
        key = (m.group(1), m.group(2), tuple(l[0].strip() for l in body if l[0].strip()))
        blocks.append((key, [lines[i]], body))
        i = j

    out = []
    for key, sig, body in blocks:
        prev = out[-1] if out else None
        if key is not None and prev is not None and prev[0] == key and key[2]:
            text, src, off = sig[0]
            m = _DIRECTIVE.match(text)
            prev[1].append((' ' * m.end() + text[m.end():], src, off))
        else:
            out.append((key, list(sig), body))

    merged = ViewList()
    for _, sig, body in out:
        for text, src, off in sig + body:
            merged.append(text, src, off)
    return merged


_get_docstrings = hawkmoth._AutoBaseDirective._AutoBaseDirective__get_docstrings
hawkmoth._AutoBaseDirective._AutoBaseDirective__get_docstrings = \
    lambda self: _merge_declarators(_get_docstrings(self))


def _inline_types(app, doctree):
    """An inline struct's type (hawkmoth_inline's struct @name), which Sphinx
    shows as [anonymous], shown as what it is: struct { ... } name."""
    from sphinx import addnodes
    for ref in list(doctree.findall(addnodes.pending_xref)):
        if ref.get('refdomain') == 'c' and ref.get('reftarget', '').split('.')[-1].startswith('@'):
            ref.replace_self(addnodes.desc_sig_punctuation('{ … }', '{ … }'))


def setup(app):
    app.connect('doctree-read', _inline_types)
    # first, before Sphinx's own filters count the warning
    for handler in logging.getLogger('sphinx').handlers:
        handler.filters.insert(0, _FileCommentFilter())
    app.connect('hawkmoth-process-docstring', _strip_last_line_prefix)
