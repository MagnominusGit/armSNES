#!/bin/sh
# Compares shared/ with the Armulators files it copies (the checkout given, ~/Armulators by default),
# so a changed copy or an Armulators update that missed one shows up.
root=$(cd "$(dirname "$0")/.." && pwd)
from=${1:-$HOME/Armulators}
status=0
while read -r copy original; do
	if ! cmp -s "$root/shared/$copy" "$from/$original"; then
		echo "differs: shared/$copy from $original"
		status=1
	fi
done <<LIST
libretro/libretro.h libretro/libretro.h
libretro/link.T libretro/link.T
LIST
[ $status = 0 ] && echo "shared/ matches $(git -C "$from" log -1 --format=%h 2>/dev/null || echo "$from")"
exit $status
