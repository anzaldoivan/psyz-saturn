#!/bin/sh
# Build libyaul and link the Saturn CI title against the PsyZ Saturn backend.
# Usage: link.sh <libyaul-src-dir>
set -eu

if [ $# -ne 1 ] || [ ! -d "$1" ]; then
    echo "usage: link.sh <libyaul-src-dir>" >&2
    exit 2
fi
LIBYAUL=$(cd "$1" && pwd)
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
TITLE=$ROOT/tools/ci/saturn
ELF=$TITLE/build/psyz-saturn-title.elf
MAP=$TITLE/build/psyz-saturn-title.map
LO=0x06004000
FIT=0x060FF000

if [ -n "${YAUL_INSTALL_ROOT:-}" ]; then
    PATH=$YAUL_INSTALL_ROOT/bin:$PATH
    export PATH
fi
PREFIX=${YAUL_ARCH_SH_PREFIX:-sh2eb-elf}

fail() {
    echo "saturn: FAIL $*" >&2
    exit 1
}

for t in gcc as ld readelf; do
    command -v "$PREFIX-$t" >/dev/null 2>&1 || fail "missing $PREFIX-$t"
done
[ -n "${YAUL_INSTALL_ROOT:-}" ] || fail "YAUL_INSTALL_ROOT is not set"

srcs=$(sed -n 's|^ *.(PSYZ_ROOT)/\([^ ]*\).*|\1|p' \
    "$ROOT/psyz/src/saturn/saturn.mk")
[ -n "$srcs" ] || fail "no sources in psyz/src/saturn/saturn.mk"
for f in $srcs tools/ci/saturn/title.c; do
    [ -f "$ROOT/$f" ] || fail "missing $f"
done

"$PREFIX-gcc" --version | head -n 1
"$PREFIX-as" --version | head -n 1
"$PREFIX-ld" --version | head -n 1

make -C "$LIBYAUL" install-release YAUL_BUILD_ROOT="$LIBYAUL" \
    YAUL_OPTION_MALLOC_IMPL=tlsf
make -C "$TITLE"

[ -f "$ELF" ] || fail "no $ELF"
[ -f "$MAP" ] || fail "no $MAP"

entry=$("$PREFIX-readelf" -h "$ELF" | sed -n 's/^ *Entry point address: *//p')
[ $((entry)) -eq $((LO)) ] || fail "entry $entry, expected $LO"

"$PREFIX-readelf" -SW "$ELF" | sed 's/^ *\[ *[0-9]*\]//' >"$TITLE/build/sections.txt"
for s in .text .rodata .data .bss; do
    grep -q "^ *\\$s " "$TITLE/build/sections.txt" || fail "no $s section"
    grep -q "^\\$s " "$MAP" || fail "no $s in map"
done

end=0
while read -r name _ addr _ size _ flg _; do
    case $flg in *A*) ;; *) continue ;; esac
    case $addr$size in '' | *[!0-9a-f]*) continue ;; esac
    [ $((0x$size)) -ne 0 ] || continue
    a=$((0x$addr & 0x0FFFFFFF))
    [ $((0x$addr >> 28)) -eq 2 ] || a=$((0x$addr))
    [ $a -ge $((LO)) ] && [ $((a + 0x$size)) -le $((FIT)) ] ||
        fail "section $name at 0x$addr size 0x$size outside [$LO, $FIT)"
    [ $((a + 0x$size)) -le $end ] || end=$((a + 0x$size))
done <"$TITLE/build/sections.txt"
[ $end -gt 0 ] || fail "no loaded sections"

mend=$(sed -n 's/^ *0x\([0-9a-f]*\) *___end = .*/\1/p' "$MAP")
[ -n "$mend" ] || fail "no ___end in map"
[ $((0x$mend)) -le $((FIT)) ] || fail "map ___end 0x$mend > $FIT"

printf 'saturn: OK entry %s end 0x%x map ___end 0x%s fit %s\n' \
    "$entry" "$end" "$mend" "$FIT"
