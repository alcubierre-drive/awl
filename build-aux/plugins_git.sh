#!/bin/sh
# plugins_git.sh SRCDIR OUT: the plugin library's git revision, "branch:hash"
# plus ":dirty" with uncommitted changes to tracked files ("undef" outside
# git), as PLUGINS_GIT in OUT, compiled into plugins.c. Rewritten only when it
# changes, so an unchanged tree doesn't relink.
src=$1 out=$2
if b=$(git -C "$src" rev-parse --abbrev-ref HEAD 2>/dev/null) &&
		h=$(git -C "$src" rev-parse --short HEAD 2>/dev/null); then
	rev="$b:$h"
	git -C "$src" diff --quiet && git -C "$src" diff --cached --quiet || rev="$rev:dirty"
else
	rev=undef
fi
echo "#define PLUGINS_GIT \"$rev\"" > "$out.tmp"
if cmp -s "$out.tmp" "$out"; then rm -f "$out.tmp"; else mv -f "$out.tmp" "$out"; fi
