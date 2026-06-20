#!/bin/sh
set -eu

outdir=${1:-build/fixtures}
root="$outdir/root"
volid=CDBENCH_FIXTURE

rm -rf "$root"
rm -f "$outdir"/cdbench-*.iso
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
    xorriso -as mkisofs -quiet -R -J -V "$volid" \
        -o "$outdir/cdbench-plain.iso" "$root"
    xorriso -as mkisofs -quiet -V "$volid" \
        -o "$outdir/cdbench-iso9660.iso" "$root"
    xorriso -as mkisofs -quiet -udf -V "$volid" \
        -o "$outdir/cdbench-udf.iso" "$root"
    xorriso -as mkisofs -quiet -R -J -udf -V "$volid" \
        -o "$outdir/cdbench-bridge.iso" "$root"
elif command -v mkisofs >/dev/null 2>&1; then
    mkisofs -quiet -R -J -V "$volid" \
        -o "$outdir/cdbench-plain.iso" "$root"
    mkisofs -quiet -V "$volid" -o "$outdir/cdbench-iso9660.iso" \
        "$root"
    if mkisofs -quiet -udf -V "$volid" \
        -o "$outdir/cdbench-udf.iso" "$root" 2>/dev/null; then
        mkisofs -quiet -R -J -udf -V "$volid" \
            -o "$outdir/cdbench-bridge.iso" "$root"
    else
        echo "mkisofs does not support -udf; skipped UDF fixtures" >&2
    fi
elif command -v genisoimage >/dev/null 2>&1; then
    genisoimage -quiet -R -J -V "$volid" \
        -o "$outdir/cdbench-plain.iso" "$root"
    genisoimage -quiet -V "$volid" -o "$outdir/cdbench-iso9660.iso" \
        "$root"
    if genisoimage -quiet -udf -V "$volid" \
        -o "$outdir/cdbench-udf.iso" "$root" 2>/dev/null; then
        genisoimage -quiet -R -J -udf -V "$volid" \
            -o "$outdir/cdbench-bridge.iso" "$root"
    else
        echo "genisoimage does not support -udf; skipped UDF fixtures" >&2
    fi
else
    echo "No ISO generator found (xorriso, mkisofs, or genisoimage)" >&2
    exit 1
fi

for iso in "$outdir"/cdbench-*.iso; do
    [ -e "$iso" ] && echo "$iso"
done
