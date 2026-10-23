"""Members declared with an inline struct or union, for hawkmoth.

hawkmoth (0.22) documents ``struct { ... } blur;`` inside a struct as an
anonymous struct of its own (a named one, ``struct PertagLayout { ... }
ptlt[32];``, as that struct): the comment above it, and the members in it,
end up under ``@anonymous_<hash>``, and ``blur`` isn't a member at all. awl's
configs are full of these, so this documents ``blur`` as a member with that
comment (of type ``struct [anonymous]``) and the members in it as
``blur.strength``: :c:member:`awl_config_t.blur.strength` resolves. Members
declared together (``int n_addr, best;``) share the comment above them, which
hawkmoth gives the first only. An
anonymous member without a name (C11: ``union { ... };``) passes its members
on as its parent's, as C does.
"""

from clang.cindex import CursorKind, TypeKind
from hawkmoth import docstring, parser

RECORDS = (CursorKind.STRUCT_DECL, CursorKind.UNION_DECL)

_recursive_parse = parser._recursive_parse


def _declared_with(field, record):
    """whether field is declared with record's type"""
    t = field._cc.type.get_canonical()
    # struct { ... } ptlt[32]: the element's
    while t.kind in (TypeKind.CONSTANTARRAY, TypeKind.INCOMPLETEARRAY):
        t = t.element_type.get_canonical()
    decl = t.get_declaration()
    return field.kind == CursorKind.FIELD_DECL and decl == record._cc


def _declared_together(before, field):
    """the first of the fields before field in the same declaration, or None"""
    start = field._cc.extent.start.offset
    for k in before:
        if k.kind == CursorKind.FIELD_DECL and k._cc.extent.start.offset == start:
            return k
    return None


def _prefixed(ds, prefix):
    if prefix:
        if ds._decl_name:
            ds._decl_name = prefix + ds._decl_name
        ds._name = prefix + ds._name if ds._name else ds._name
    return ds


def _members(errors, record, nest, prefix):
    """record's documented members, nest deep, their names prefixed"""
    out = []
    kids = list(record.get_children())
    def inline(i, c):
        # struct { ... } blur; struct PertagLayout { ... } ptlt[32];
        return c.kind in RECORDS and (c._cc.is_anonymous() or
                                      (i + 1 < len(kids) and _declared_with(kids[i + 1], c)))

    for i, c in enumerate(kids):
        if inline(i, c):
            fields = []
            for f in kids[i + 1:]:
                if not _declared_with(f, c):
                    break
                fields.append(f)
            if not fields:
                # a member without a name: its members are the parent's
                out += _members(errors, c, nest, prefix)
                continue
            kind = 'union' if c.kind == CursorKind.UNION_DECL else 'struct'
            for f in fields:
                name = f._cc.spelling
                inner = _members(errors, c, nest, f'{prefix}{name}.')
                # without a comment too if its members have one: Sphinx
                # wants the parent of blur.strength declared
                if c.comment or inner:
                    ds = docstring.MemberDocstring(cursor=f, nest=nest)
                    ds._text = c.comment or '/** */'
                    # the line the comment starts on, else the struct's
                    ds._meta = dict(c.meta, line=c.line or c.location.line)
                    ds._ttype = f'{kind} @{name}'
                    out.append(_prefixed(ds, prefix))
                out += inner
        elif (c.kind == CursorKind.FIELD_DECL and not c.comment and
              (first := _declared_together(kids[:i], c)) and first.comment):
            ds = docstring.MemberDocstring(cursor=c, nest=nest)
            ds._text = first.comment
            ds._meta = dict(c.meta, line=first.line)
            out.append(_prefixed(ds, prefix))
        elif c.comment and not (c.kind == CursorKind.FIELD_DECL and
                                any(inline(j, k) and _declared_with(c, k)
                                    for j, k in enumerate(kids[:i]))):
            out += [_prefixed(ds, prefix) for ds in parser._recursive_parse(errors, c, nest)]
    return out


def _parse(errors, cursor, nest):
    if cursor.kind not in RECORDS:
        return _recursive_parse(errors, cursor, nest)
    if cursor.kind == CursorKind.STRUCT_DECL:
        ds = docstring.StructDocstring(cursor=cursor, nest=nest)
    else:
        ds = docstring.UnionDocstring(cursor=cursor, nest=nest)
    ds.add_children(_members(errors, cursor, nest + 1, ''))
    return [ds]


def setup(app):
    parser._recursive_parse = _parse
    return {'parallel_read_safe': True}
