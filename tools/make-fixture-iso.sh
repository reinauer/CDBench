#!/bin/sh
set -eu

outdir=${1:-build/fixtures}
root="$outdir/root"
iso="$outdir/cdbench-plain.iso"
volid=CDBENCH_FIXTURE

rm -rf "$root"
mkdir -p "$root/bench/tree/deep/a/b/c" "$root/bench/tree/wide"

dd if=/dev/zero of="$root/bench/seq.bin" bs=1024 count=32768 2>/dev/null
dd if=/dev/zero of="$root/bench/rand.bin" bs=1024 count=8192 2>/dev/null
dd if=/dev/zero of="$root/bench/cache-1m.bin" bs=1024 count=1024 2>/dev/null
dd if=/dev/zero of="$root/bench/cache-2m.bin" bs=1024 count=2048 2>/dev/null
dd if=/dev/zero of="$root/bench/cache-4m.bin" bs=1024 count=4096 2>/dev/null
dd if=/dev/zero of="$root/bench/cache-8m.bin" bs=1024 count=8192 2>/dev/null

i=1
while [ "$i" -le 128 ]; do
    printf 'small fixture %03d\n' "$i" > "$root/bench/tree/wide/file$(printf '%03d' "$i").txt"
    i=$((i + 1))
done

printf 'deep fixture\n' > "$root/bench/tree/deep/a/b/c/leaf.txt"
cat > "$root/README.TXT" <<EOF
CDBench fixture ISO

Volume: $volid
Sequential target: bench/seq.bin, 32 MiB
Random target: bench/rand.bin, 8 MiB
Small-file target: bench/tree/wide
Deep-directory target: bench/tree/deep/a/b/c
EOF

mkdir -p "$outdir"
if command -v xorriso >/dev/null 2>&1; then
    xorriso -as mkisofs -quiet -R -J -V "$volid" -o "$iso" "$root"
elif command -v mkisofs >/dev/null 2>&1; then
    mkisofs -quiet -R -J -V "$volid" -o "$iso" "$root"
elif command -v genisoimage >/dev/null 2>&1; then
    genisoimage -quiet -R -J -V "$volid" -o "$iso" "$root"
else
    echo "No ISO generator found (xorriso, mkisofs, or genisoimage)" >&2
    exit 1
fi

echo "$iso"
