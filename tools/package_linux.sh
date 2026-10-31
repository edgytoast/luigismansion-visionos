#!/bin/bash
# Builds and packages the Linux release (docs/LINUX.md):
#   tools/package_linux.sh <LLVM bin dir> [out dir]
# <LLVM bin dir>: bin/ of the official LLVM release for Linux x86_64 (LLVM-<ver>-Linux-X64),
# whose clang and ld.lld are bundled. Produces <out>/LuigisMansionRecompiled-linux-x86_64/ plus
# .tar.gz and .zip of it. No game data is involved.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LLVM_BIN=${1:?usage: package_linux.sh <LLVM bin dir> [out dir]}
OUT=${2:-$ROOT/out}
NAME=LuigisMansionRecompiled-linux-x86_64
BUILD=$ROOT/build/linux-clang

[ -x "$LLVM_BIN/clang" ] && [ -e "$LLVM_BIN/ld.lld" ] || { echo "no clang/ld.lld in $LLVM_BIN" >&2; exit 1; }

cd "$ROOT"
cmake --preset linux-clang -DLM_LLVM_DIR="$LLVM_BIN" > /dev/null
cmake --build "$BUILD" --target LuigisMansionRecomp lm_libxml2_stub

rm -rf "${OUT:?}/$NAME"
mkdir -p "$OUT"
cmake --install "$BUILD" --prefix "$OUT/$NAME" --strip

# Sanity checks: what the package needs from the system.
echo "== $NAME"
(cd "$OUT/$NAME" && find . -type f -printf '%m %10s %p\n' | sort -k3)
echo "== required glibc / libstdc++ versions"
for b in "$OUT/$NAME/LuigisMansionRecomp" "$OUT/$NAME/llvm/bin/clang" "$OUT/$NAME/llvm/bin/ld.lld"; do
    printf '%s: ' "${b#$OUT/}"
    objdump -T "$b" | grep -o 'GLIBC_[0-9.]*\|GLIBCXX_[0-9.]*' | sort -uV | awk -F_ '{m[$1]=$0} END {printf "%s %s\n", m["GLIBC"], m["GLIBCXX"]}'
    readelf -d "$b" | grep NEEDED | sed 's/.*\[\(.*\)\]/  needs \1/' | tr '\n' ' '; echo
done
"$OUT/$NAME/llvm/bin/clang" --version | head -1
"$OUT/$NAME/llvm/bin/ld.lld" --version
# The release must load on glibc 2.34 (docs/LINUX.md "Portable executable"): fail on newer
# symbol versions (add a shim to src/port/glibc_compat.c).
for b in "$OUT/$NAME/LuigisMansionRecomp" "$OUT/$NAME/llvm/bin/clang" "$OUT/$NAME/llvm/bin/ld.lld"; do
    if objdump -T "$b" | grep -E 'GLIBC_2\.(3[5-9]|[4-9][0-9])'; then
        echo "${b#$OUT/} needs glibc newer than 2.34 (symbols above)" >&2
        exit 1
    fi
done

cd "$OUT"
rm -f "$NAME.tar.gz" "$NAME.zip"
tar -czf "$NAME.tar.gz" --owner=0 --group=0 "$NAME"
zip -qry "$NAME.zip" "$NAME"
ls -la "$NAME.tar.gz" "$NAME.zip"
du -sh "$NAME"
