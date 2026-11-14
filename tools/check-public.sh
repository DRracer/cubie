#!/bin/sh
# SPDX-License-Identifier: MIT
# Consistency check of the patches in this repository; run from the repository root before a push.
# Every patch must name the project identity (From: or a DEP-3 Author:) and carry exactly one
# Assisted-by trailer (cover letters excepted); no Co-Authored-By lines, placeholders or local
# paths. This checks headers, it does not prove that a file is free of private details.
set -u
ID='"D.R.racer" <drracer@seznam.cz>'
ASSIST='Assisted-by: Claude:claude-opus-5-5'
bad=0
for f in $(git ls-files '*.patch' '*.diff'); do
	ids=$(grep -E '^(From|Author): ' "$f")
	if [ -z "$ids" ]; then
		echo "$f: no From:/Author: line"; bad=1
	elif echo "$ids" | sed -E 's/^(From|Author): //' | grep -vqxF -e "$ID" -e 'D.R.racer <drracer@seznam.cz>'; then
		echo "$f: unexpected author"; bad=1
	fi
	if grep -Eq '^Subject: \[(RFC )?PATCH[^]]* 0+/[0-9]+\]' "$f"; then
		continue
	fi
	n=$(grep -cxF "$ASSIST" "$f")
	[ "$n" -eq 1 ] || { echo "$f: $n Assisted-by trailers"; bad=1; }
done
if git grep -nIE 'Co-Authored-By|Assisted-by: LLM|PENDING|(^|[[:space:](])/(home|media|mnt)/' -- . ':!tools/check-public.sh'; then
	bad=1
fi
[ "$bad" -eq 0 ] && echo "check-public: OK"
exit "$bad"
