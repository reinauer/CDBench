#!/usr/bin/env bash
#
# build-cdfs-bench.sh - assemble a single Amiga floppy image (ADF) that holds
# every CD filesystem found as an archive in this directory, so each one can be
# mounted as CD0: and benchmarked with cdbench under WinUAE.
#
# For every known CDFS archive the script:
#   * extracts the actual filesystem handler binary (unpacking zip / lha / dms /
#     tar.bz2 / nested install-ADFs as needed),
#   * drops it into its own drawer on the image as <drawer>/L/<handler>,
#   * writes a matching CD0 mount entry and a "double-click to mount" CD0.info,
#   * writes a small "Bench" script that mounts CD0: and runs cdbench.
#
# The cdbench "tr" build is bundled at the volume root as CDBench, and results
# land in <VOL>:Results/<drawer>.txt.
#
# This tool lives in ~/CDFS and is intentionally NOT part of the ODFileSystem
# tree.  Nothing here is copied into that repository.
#
# Usage:
#     ./build-cdfs-bench.sh
#
# Override anything with an environment variable, e.g.:
#     CD_DEVICE=uaescsi.device CD_UNIT=0 ./build-cdfs-bench.sh
#
set -euo pipefail

# --------------------------------------------------------------------------
# Configuration (override via environment)
# --------------------------------------------------------------------------
CDFS_DIR="${CDFS_DIR:-$HOME/CDFS}"
OUT_ADF="${OUT_ADF:-$CDFS_DIR/CDFSBench.adf}"
VOLNAME="${VOLNAME:-CDFSBench}"

# Exec device + unit of the CD/DVD drive as seen by AmigaOS in your emulator.
# These go into every generated CD0 mount entry - override to match your setup,
# e.g. CD_DEVICE=uaescsi.device CD_UNIT=0 ./build-cdfs-bench.sh
CD_DEVICE="${CD_DEVICE:-a4091.device}"
CD_UNIT="${CD_UNIT:-2}"

# The cdbench binary to bundle.
CDBENCH_BIN="${CDBENCH_BIN:-$HOME/git/cdbench/build/CDBench}"

# Tool locations (auto-detected below if left at defaults).
XDFTOOL="${XDFTOOL:-}"
LHA="${LHA:-}"
XDMS="${XDMS:-}"
XDMS_SRC="${XDMS_SRC:-$HOME/old/git/xdms/xdms_new/src}"

# --------------------------------------------------------------------------
# Plumbing
# --------------------------------------------------------------------------
log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cdfsbench.XXXXXX")"
EX="$WORK/ex"          # extracted handlers / icon land here
mkdir -p "$EX"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# Locate a required host tool, honouring an explicit override.
find_tool() { # varname candidate...
    local var="$1"; shift
    local cur="${!var}"
    if [ -n "$cur" ] && [ -x "$cur" ]; then return 0; fi
    local c
    for c in "$@"; do
        if command -v "$c" >/dev/null 2>&1; then printf -v "$var" '%s' "$(command -v "$c")"; return 0; fi
        [ -x "$c" ] && { printf -v "$var" '%s' "$c"; return 0; }
    done
    return 1
}

# First path matching a glob, or empty. Handles spaces/UTF-8 in names.
first_match() { # glob
    local m
    for m in $1; do [ -e "$m" ] && { printf '%s' "$m"; return 0; }; done
    return 1
}

xdf() { "$XDFTOOL" "$OUT_ADF" "$@"; }

# --------------------------------------------------------------------------
# Tool discovery
# --------------------------------------------------------------------------
step "Locating host tools"
find_tool XDFTOOL xdftool "$HOME/Library/Python/3.14/bin/xdftool" \
    || die "xdftool (amitools) not found - install with: pip3 install amitools"
find_tool LHA lha lhasa "$HOME/bin/lha" \
    || die "lha not found (needed to unpack .lha archives)"
log "xdftool: $XDFTOOL"
log "lha:     $LHA"

command -v unzip >/dev/null || die "unzip not found"
command -v tar   >/dev/null || die "tar not found"

# xdms is needed for the CacheCDFS .dms archive; build it from source if absent.
if ! find_tool XDMS xdms; then
    if [ -d "$XDMS_SRC" ]; then
        log "building xdms from $XDMS_SRC"
        ( cd "$XDMS_SRC" && cc -O2 -w -o "$WORK/xdms" \
            crc_csum.c getbits.c maketbl.c pfile.c tables.c \
            u_deep.c u_heavy.c u_init.c u_medium.c u_quick.c u_rle.c xdms.c ) \
            && XDMS="$WORK/xdms"
    fi
fi
[ -n "${XDMS:-}" ] && log "xdms:    $XDMS" || warn "xdms unavailable - CacheCDFS .dms will be skipped"

[ -f "$CDBENCH_BIN" ] || die "cdbench binary not found at: $CDBENCH_BIN"

# --------------------------------------------------------------------------
# Archive helpers
# --------------------------------------------------------------------------

# Unpack the single install-ADF that lives inside a zip, echo its path.
adf_from_zip() { # zipglob destdir
    local zip; zip="$(first_match "$1")" || return 1
    local dst="$2"; mkdir -p "$dst"
    unzip -o -qq "$zip" -d "$dst"
    first_match "$dst/*.adf"
}

# Read one file out of an ADF to a host path.
adf_read() { "$XDFTOOL" "$1" read "$2" "$3"; }

# Extract exactly one member from an lha archive into a dir, echo its path.
lha_one() { # archive member destdir
    local dst="$3"; mkdir -p "$dst"
    ( cd "$dst" && "$LHA" xfq "$1" "$2" >/dev/null 2>&1 )
    printf '%s' "$dst/$2"
}

# --------------------------------------------------------------------------
# Per-CDFS recipes
# --------------------------------------------------------------------------
# Each prepare_<name> sets these globals and leaves the handler binary on disk:
#   R_NAME     drawer name on the image
#   R_VER      human version string (for README/summary)
#   R_FSFILE   handler filename inside <drawer>/L
#   R_HANDLER  host path to the extracted handler binary
#   R_DOSTYPE  mount DosType (0x........)
#   R_CONTROL  Control="" string (empty = omit)
#   R_BUFFERS R_BUFMEMTYPE R_MASK R_HIGHCYL R_STACKSIZE   mount tuning
#   R_DEVS     newline list "hostpath destname" of extra files for <drawer>/Devs
#   R_NOTE     one-line note shown in the summary (e.g. "DosType unverified")
# Returns non-zero (and warns) if the source archive is missing.

reset_recipe() {
    R_NAME="$1"; R_VER=""; R_FSFILE=""; R_HANDLER=""; R_DOSTYPE="0x43443031"
    R_CONTROL=""; R_BUFFERS=64; R_BUFMEMTYPE=0; R_MASK="0x7ffffffe"
    R_HIGHCYL=27000; R_STACKSIZE=8192; R_DEVS=""; R_NOTE=""
}

prepare_AllegroCDFS() {
    reset_recipe AllegroCDFS
    local src; src="$(first_match "$CDFS_DIR/AllegroCDFS")" || { warn "AllegroCDFS: source missing"; return 1; }
    R_VER="3.9"; R_FSFILE="AllegroCDFS"
    R_HANDLER="$EX/AllegroCDFS.bin"; cp "$src" "$R_HANDLER"
    R_DOSTYPE="0x41434453"   # 'ACDS'
}

prepare_BabelCDFS() {
    reset_recipe BabelCDFS
    local src; src="$(first_match "$CDFS_DIR/BabelCDFS.tar.bz2")" || { warn "BabelCDFS: source missing"; return 1; }
    R_VER="1.2"; R_FSFILE="BABELCDROMFS"
    local d="$EX/babel"; mkdir -p "$d"; tar xjf "$src" -C "$d"
    R_HANDLER="$d/BABELCDROMFS"
    R_NOTE="DosType unverified (edit CD0 if disc not recognised)"
}

prepare_AmiCDFS() {
    reset_recipe AmiCDFS
    local src; src="$(first_match "$CDFS_DIR/amicdfs240.lha")" || { warn "AmiCDFS: source missing"; return 1; }
    R_VER="2.40"; R_FSFILE="AmiCDFS"
    local d="$EX/amicdfs"; mkdir -p "$d"
    ( cd "$d" && "$LHA" xfq "$src" >/dev/null 2>&1 )
    R_HANDLER="$d/AmiCDFS2/L/AmiCDFS"
    # Values taken from the bundled AmiCDFS2/CD0 mount entry.
    R_DOSTYPE="0x43444653"; R_CONTROL="LC BL=8 FB=32"
    R_BUFMEMTYPE=1; R_MASK="0x7fffffff"; R_HIGHCYL=11000; R_STACKSIZE=600
}

prepare_AsimCDFS() {
    reset_recipe AsimCDFS
    local adf; adf="$(adf_from_zip "$CDFS_DIR/AsimCDFS*v3.10*.zip" "$EX/asim_zip")" \
        || { warn "AsimCDFS: source zip missing"; return 1; }
    R_VER="3.10"; R_FSFILE="AsimCDFS"
    # Handler lives in l/AsimCDFS.lha; the ATAPI helper in devs/asimcdfs.lha.
    adf_read "$adf" l/AsimCDFS.lha "$EX/AsimCDFS.lha"
    R_HANDLER="$(lha_one "$EX/AsimCDFS.lha" AsimCDFS "$EX/asim_l")"
    adf_read "$adf" devs/asimcdfs.lha "$EX/asimcdfs_dev.lha"
    local dev; dev="$(lha_one "$EX/asimcdfs_dev.lha" asimcdfs.device "$EX/asim_dev")"
    R_DEVS="$dev asimcdfs.device"
    # Values taken from the bundled devs/MountList.CD0 entry.
    R_DOSTYPE="0x662dabac"; R_BUFFERS=192; R_BUFMEMTYPE=5
    R_MASK="0xffffffe"; R_HIGHCYL=999; R_STACKSIZE=5000
}

prepare_CacheCDFS() {
    reset_recipe CacheCDFS
    local adf; adf="$(adf_from_zip "$CDFS_DIR/CacheCDFS*v2.11*.zip" "$EX/cache_zip")" \
        || { warn "CacheCDFS: source zip missing"; return 1; }
    R_VER="2.11 (106.1)"; R_FSFILE="CacheCDFS"
    R_HANDLER="$EX/CacheCDFS"; adf_read "$adf" l/CacheCDFS "$R_HANDLER"
    R_CONTROL="MD=0 LC=1 DC=8 L LV AL LFC=1 S NC"   # required, or the handler will not mount
    # Stash the generic CD0.info icon from this package for reuse everywhere.
    [ -f "$EX/CD0.info" ] || adf_read "$adf" devs/DosDrivers/CD0.info "$EX/CD0.info" || true
}

prepare_CacheCDFS43() {
    reset_recipe CacheCDFS43
    [ -n "${XDMS:-}" ] || { warn "CacheCDFS43: xdms unavailable, skipping"; return 1; }
    local src; src="$(first_match "$CDFS_DIR/cachecdfs-v43.4.dms")" || { warn "CacheCDFS43: source missing"; return 1; }
    R_VER="43.4"; R_FSFILE="CacheCDFS"
    local adf="$EX/cache434.adf"
    "$XDMS" u "$src" +"$adf" >/dev/null 2>&1 || { warn "CacheCDFS43: xdms failed"; return 1; }
    R_HANDLER="$EX/CacheCDFS43"; adf_read "$adf" l/CacheCDFS "$R_HANDLER"
    R_CONTROL="MD=0 LC=1 DC=8 L LV AL LFC=1 S NC"   # required, or the handler will not mount
}

# Ordered list of recipes to attempt.
RECIPES=(AllegroCDFS AmiCDFS AsimCDFS BabelCDFS CacheCDFS CacheCDFS43)

# --------------------------------------------------------------------------
# Emit a CD0 mount entry for the current recipe.
# --------------------------------------------------------------------------
write_mountlist() { # outfile
    {
        cat <<EOF
/* CD0 mount entry for $R_NAME $R_VER
 *
 * Generated by build-cdfs-bench.sh.  Double-click CD0.info (or run
 * "Mount CD0" from this drawer) to make the drive available as CD0:.
 * If a disc is not recognised, adjust Device/Unit or DosType below.
 */
Device      = $CD_DEVICE
Unit        = $CD_UNIT
FileSystem  = $VOLNAME:$R_NAME/L/$R_FSFILE
Flags       = 0
Surfaces    = 1
BlocksPerTrack = 32
BlockSize   = 2048
Reserved    = 0
Interleave  = 0
LowCyl      = 0
HighCyl     = $R_HIGHCYL
Buffers     = $R_BUFFERS
BufMemType  = $R_BUFMEMTYPE
Mask        = $R_MASK
MaxTransfer = 0x000fffff
StackSize   = $R_STACKSIZE
Priority    = 10
GlobVec     = -1
DosType     = $R_DOSTYPE
EOF
        [ -n "$R_CONTROL" ] && printf 'Control     = "%s"\n' "$R_CONTROL"
        printf '#\n'
    } > "$1"
}

# Emit the per-drawer Bench script (AmigaDOS).
write_bench() { # outfile
    cat > "$1" <<EOF
.KEY passopts
; Benchmark $R_NAME $R_VER.
; Insert a CD in the emulated drive, then from a Shell run:
;     $VOLNAME:$R_NAME/Bench
CD $VOLNAME:$R_NAME
Mount CD0
$VOLNAME:CDBench DEVICE CD0: <passopts> >$VOLNAME:Results/$R_NAME.txt
Type $VOLNAME:Results/$R_NAME.txt
EOF
}

# --------------------------------------------------------------------------
# Build the image
# --------------------------------------------------------------------------
step "Creating image $OUT_ADF (volume $VOLNAME)"
rm -f "$OUT_ADF"
xdf create + format "$VOLNAME" ffs >/dev/null

# Pre-extract the CacheCDFS package first so the shared icon is available even
# if CacheCDFS itself is later skipped for any reason.
prepare_CacheCDFS >/dev/null 2>&1 || true

BUILT=()   # "name|ver|fsfile|dostype|size|note"
for name in "${RECIPES[@]}"; do
    step "Staging $name"
    if ! "prepare_$name"; then
        log "skipped (source unavailable)"
        continue
    fi
    [ -f "$R_HANDLER" ] || { warn "$name: handler not extracted, skipping"; continue; }

    xdf makedir "$R_NAME" >/dev/null
    xdf makedir "$R_NAME/L" >/dev/null
    xdf write "$R_HANDLER" "$R_NAME/L/$R_FSFILE" >/dev/null
    log "handler -> $R_NAME/L/$R_FSFILE ($(stat -f%z "$R_HANDLER") bytes)"

    write_mountlist "$WORK/CD0"
    xdf write "$WORK/CD0" "$R_NAME/CD0" >/dev/null
    if [ -f "$EX/CD0.info" ]; then
        xdf write "$EX/CD0.info" "$R_NAME/CD0.info" >/dev/null
    fi

    write_bench "$WORK/Bench"
    xdf write "$WORK/Bench" "$R_NAME/Bench" >/dev/null
    xdf protect "$R_NAME/Bench" +se >/dev/null

    # Optional extra device files (e.g. Asim's asimcdfs.device).
    if [ -n "$R_DEVS" ]; then
        xdf makedir "$R_NAME/Devs" >/dev/null
        # shellcheck disable=SC2086
        set -- $R_DEVS
        while [ "$#" -ge 2 ]; do
            xdf write "$1" "$R_NAME/Devs/$2" >/dev/null
            log "devs -> $R_NAME/Devs/$2"
            shift 2
        done
    fi

    BUILT+=("$R_NAME|$R_VER|$R_FSFILE|$R_DOSTYPE|$(stat -f%z "$R_HANDLER")|$R_NOTE")
done

[ "${#BUILT[@]}" -gt 0 ] || die "no CD filesystems were staged - nothing to build"

step "Bundling cdbench and top-level files"
xdf makedir Results >/dev/null
xdf write "$CDBENCH_BIN" CDBench >/dev/null
xdf protect CDBench +e >/dev/null
log "CDBench -> $VOLNAME:CDBench ($(stat -f%z "$CDBENCH_BIN") bytes)"

# Top-level README describing the workflow.
{
    echo "$VOLNAME - CD filesystem benchmark disk"
    echo "Built $(date '+%Y-%m-%d %H:%M') by build-cdfs-bench.sh"
    echo
    echo "Each drawer holds one CD filesystem plus a CD0 mount entry."
    echo "To benchmark one:"
    echo "  1. Insert a CD in the emulated drive ($CD_DEVICE unit $CD_UNIT)."
    echo "  2. Double-click the drawer's CD0 icon (or: Mount CD0)."
    echo "  3. From a Shell:  $VOLNAME:CDBench DEVICE CD0:"
    echo "     or just run the drawer's Bench script; results go to Results/."
    echo "  4. Reboot (or unmount CD0:) before testing another filesystem."
    echo
    echo "Filesystems on this disk:"
    for row in "${BUILT[@]}"; do
        IFS='|' read -r n v f d s note <<<"$row"
        printf '  %-14s %-12s DosType=%-10s  %s\n' "$n" "$v" "$d" "$note"
    done
} > "$WORK/README"
xdf write "$WORK/README" README >/dev/null

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
step "Done: $OUT_ADF"
printf '  %-14s %-12s %-14s %-12s %8s  %s\n' NAME VERSION HANDLER DOSTYPE BYTES NOTE
for row in "${BUILT[@]}"; do
    IFS='|' read -r n v f d s note <<<"$row"
    printf '  %-14s %-12s %-14s %-12s %8s  %s\n' "$n" "$v" "$f" "$d" "$s" "$note"
done
echo
"$XDFTOOL" "$OUT_ADF" info
echo
log "Device/Unit in every CD0 entry: $CD_DEVICE unit $CD_UNIT"
log "Re-run with e.g. CD_DEVICE=uaescsi.device CD_UNIT=0 to change that."
