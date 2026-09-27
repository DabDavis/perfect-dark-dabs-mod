#!/bin/sh
# Turns patchnotes.txt into the Markdown body of a GitHub release: the intro
# line given as $2, then the newest $3 entries (default 5), newest first, then
# a pointer to the rest. The release job runs it for the rolling dev build so
# the release page says what the build brings, the same lines the game shows
# on Check for Updates. Lines starting with # and anything above the first
# "notes" line are skipped, as patchnotes.c skips them.
#
#   tools/patchnotes-md.sh patchnotes.txt "intro text" [entries] > body.md

file="$1"
intro="$2"
max="${3:-5}"

printf '%s\n\n## What'"'"'s new\n' "$intro"
awk -v max="$max" '
	/^#/ { next }
	/^notes[ \t]/ {
		n++
		if (n > max) { more = 1; exit }
		printf "\n### Update %s (%s)\n\n", $2, $3
		inentry = 1
		next
	}
	!inentry { next }
	/^[ \t]*$/ { next }
	{ print "- " $0 }
	END { if (more) print "\nOlder updates are in patchnotes.txt inside the download." }
' "$file"
