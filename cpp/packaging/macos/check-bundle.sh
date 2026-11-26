#!/usr/bin/env bash
# CI check of a deployed LibreRaman.app: every Mach-O file contains all architectures, needs at most the
# given macOS version, and links only system libraries or libraries inside the bundle.
#   check-bundle.sh LibreRaman.app 12.0 "arm64 x86_64"
set -uo pipefail
app=$1 min=$2 archs=$3
fail=0 n=0
newer() {   # 0 if version $1 > $2
    awk -v a="$1" -v b="$2" 'BEGIN { split(a, x, "."); split(b, y, ".");
        for (i = 1; i <= 3; i++) { if (x[i] + 0 > y[i] + 0) exit 0; if (x[i] + 0 < y[i] + 0) exit 1 } exit 1 }'
}
while IFS= read -r -d '' f; do
    file -b "$f" | grep -q 'Mach-O' || continue
    n=$((n + 1))
    # shellcheck disable=SC2086
    lipo "$f" -verify_arch $archs || { echo "ERROR: $f lacks one of: $archs"; fail=1; }
    for a in $archs; do
        m=$(otool -arch "$a" -l "$f" | awk '/LC_BUILD_VERSION/ {b=1} b && $1 == "minos" {print $2; exit}
                                           /LC_VERSION_MIN_MACOSX/ {v=1} v && $1 == "version" {print $2; exit}')
        if [ -z "$m" ] || newer "$m" "$min"; then echo "ERROR: $f ($a) needs macOS ${m:-?} > $min"; fail=1; fi
    done
    id=$(otool -D "$f" | sed -n 2p)
    while read -r dep; do
        [ "$dep" = "$id" ] && continue
        case $dep in
            /System/* | /usr/lib/* | @rpath/* | @executable_path/* | @loader_path/*) ;;
            *) echo "ERROR: $f links $dep (outside the bundle)"; fail=1 ;;
        esac
    done < <(otool -L "$f" | grep $'^\t' | awk '{print $1}' | sort -u)
done < <(find "$app" -type f -print0)
echo "checked $n Mach-O files in $app"
[ "$n" -gt 0 ] || fail=1
exit $fail
