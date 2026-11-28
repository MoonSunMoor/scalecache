#!/bin/bash
# Generate the pxt4 and jbd3 out-of-tree modules from a kernel's fs/ext4 and fs/jbd2.
#   scripts/pxt4-gen/gen.sh [-n] <linux repo> <rev> <out dir>
#     <rev>  tag or commit whose fs/ext4 and fs/jbd2 are used (v7.2.8)
#     -n     stop after the rename, without the ScaleCache patches
# Result: <out dir>/pxt4 (pxt4.ko) with <out dir>/pxt4/jbd3 (jbd3.ko).
# Build:  make -C <out dir>/pxt4 KDIR=<kernel build dir>
set -eu
GEN=$(cd "$(dirname "$0")" && pwd)
RENAME_ONLY=0
[ "${1:-}" = -n ] && { RENAME_ONLY=1; shift; }
[ $# -eq 3 ] || { sed -n '2,8p' "$0"; exit 1; }
REPO=$(cd "$1" && pwd) REV=$2 OUT=$3

[ -e "$OUT" ] && { echo "$OUT exists"; exit 1; }
mkdir -p "$OUT"
git -C "$REPO" archive "$REV" fs/ext4 fs/jbd2 | tar -x -C "$OUT"
mv "$OUT/fs/ext4" "$OUT/pxt4"
mv "$OUT/fs/jbd2" "$OUT/pxt4/jbd3"
rmdir "$OUT/fs"
cd "$OUT/pxt4"

# The rename rule of the pxt4-support headers (include/linux/jbd3.h,
# journal-head3.h, include/trace/events/{jbd3,pxt4}.h, uapi pxt4.h).
for f in $(find . -name '*ext4*' -o -name '*jbd2*'); do
	mv "$f" "$(dirname "$f")/$(basename "$f" | sed 's/jbd2/jbd3/g; s/ext4/pxt4/g')"
done
find . -type f -print0 | xargs -0 sed -i \
	's/jbd2/jbd3/g; s/JBD2/JBD3/g; s/ext4/pxt4/g; s/EXT4/PXT4/g; s/Ext4/Pxt4/g; s/journal-head\.h/journal-head3.h/g'
# EXTN_FEATURE_FUNCS(ver) in pxt4.h pastes EXT##ver## and ext##ver##, so the
# ext2/ext3 instances follow the same prefix as the renamed ext4 one.
find . -type f -print0 | xargs -0 sed -i \
	's/EXT##ver##_/PXT##ver##_/g; s/_ext##ver##_/_pxt##ver##_/g;
	 s/EXT\([23]\)_FEATURE_\([A-Z_]*\)SUPP/PXT\1_FEATURE_\2SUPP/g;
	 s/has_unknown_ext\([23]\)_/has_unknown_pxt\1_/g'
[ $RENAME_ONLY = 1 ] && exit 0

# ScaleCache changes, in order.
for p in "$GEN"/patches/*.patch; do
	patch -p1 -s --no-backup-if-mismatch < "$p"
done
echo "generated $OUT/pxt4 from $(git -C "$REPO" rev-parse --short=12 "$REV^{commit}") ($REV)"
