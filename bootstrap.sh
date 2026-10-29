#!/bin/sh
# Innova Core - chain bootstrap installer.
#
# PURPOSE
#   Download a published blockchain bootstrap archive and install its chain
#   data (blk*.dat and the chain state it carries) into an Innova datadir, so a
#   new node starts near the chain tip instead of syncing from genesis.
#
# CONTRACT
#   * Runs as the invoking user. No root, no sudo, no package installation.
#   * Never touches wallet.dat, innova.conf, masternode.conf or wallet backups.
#     Only chain artifacts are replaced.
#   * Existing chain artifacts are MOVED to <datadir>/bootstrap-backup-<ts>/
#     before anything new is written, so an install is always reversible.
#   * Refuses to run while innovad is running (never kills the daemon).
#   * Works from any working directory: paths derive from the script location
#     and from explicit arguments.
#   * Idempotent: a completed install writes a marker; a re-run without --force
#     reports the existing install and exits 0.
#   * Any failure exits non-zero with a diagnostic on stderr.
#
# USAGE
#   ./bootstrap.sh [options]
#     -d, --datadir DIR   target datadir (default: $HOME/.innova)
#     -a, --archive FILE  use a local archive instead of downloading
#     -u, --url URL       override the download URL
#         --sha256 HEX    verify the archive digest before extracting
#         --force         reinstall even if a previous install is recorded
#         --start         start innovad with -datadir after installing
#         --keep-archive  keep the downloaded archive in <datadir>/
#     -h, --help          show this help
#
# EXIT CODES
#   0 success (including "already installed")
#   1 usage error
#   2 missing dependency
#   3 environment / daemon conflict
#   4 download failure
#   5 archive or verification failure
#   6 install failure

set -eu

PROG=$(basename "$0")

# Resolve the project root from the script itself so the script works from any
# working directory (symlinks followed where readlink supports it).
SCRIPT_PATH=$0
if command -v readlink >/dev/null 2>&1; then
    RESOLVED=$(readlink -f -- "$SCRIPT_PATH" 2>/dev/null || true)
    [ -n "${RESOLVED:-}" ] && SCRIPT_PATH=$RESOLVED
fi
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd -P)

DEFAULT_URL="https://github.com/innova-foundation/innova/releases/download/v4.3.9.5/innovabootstrap.rar"
DATADIR="${HOME:-}/.innova"
URL=$DEFAULT_URL
ARCHIVE=""
WANT_SHA256=""
FORCE=0
START=0
KEEP_ARCHIVE=0

# Wallet/config files are NEVER replaced by a chain bootstrap.
# Chain artifacts a bootstrap is allowed to replace.
CHAIN_ITEMS="blk0001.dat blkindex.dat txleveldb chainstate database"

usage() {
    sed -n '2,45p' "$SCRIPT_PATH" | sed 's/^# \{0,1\}//'
}

die() {
    code=$1
    shift
    printf '%s: error: %s\n' "$PROG" "$*" >&2
    exit "$code"
}

info() {
    printf '%s\n' "$*"
}

# ---------------------------------------------------------------- arguments
while [ $# -gt 0 ]; do
    case $1 in
        -d|--datadir)
            [ $# -ge 2 ] || die 1 "--datadir requires a value"
            DATADIR=$2
            shift 2
            ;;
        -a|--archive)
            [ $# -ge 2 ] || die 1 "--archive requires a value"
            ARCHIVE=$2
            shift 2
            ;;
        -u|--url)
            [ $# -ge 2 ] || die 1 "--url requires a value"
            URL=$2
            shift 2
            ;;
        --sha256)
            [ $# -ge 2 ] || die 1 "--sha256 requires a value"
            WANT_SHA256=$2
            shift 2
            ;;
        --force)
            FORCE=1
            shift
            ;;
        --start)
            START=1
            shift
            ;;
        --keep-archive)
            KEEP_ARCHIVE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die 1 "unknown option: $1 (try --help)"
            ;;
    esac
done

[ -n "${HOME:-}" ] || die 1 "HOME is not set; pass --datadir explicitly"
[ -n "$DATADIR" ] || die 1 "empty datadir"

# ------------------------------------------------------------ project layout
if [ ! -f "$PROJECT_ROOT/src/makefile.unix" ]; then
    info "warning: $PROJECT_ROOT does not look like the Innova Core source root" >&2
    info "         (src/makefile.unix not found); continuing with --datadir only" >&2
fi

# Resolve the datadir to an absolute path before any directory change.
case $DATADIR in
    /*) : ;;
    *)
        DATADIR_DIR=$(dirname -- "$DATADIR")
        mkdir -p -- "$DATADIR_DIR" || die 1 "cannot create parent of datadir: $DATADIR_DIR"
        DATADIR=$(CDPATH= cd -- "$DATADIR_DIR" && pwd -P)/$(basename -- "$DATADIR")
        ;;
esac

if [ ! -d "$DATADIR" ]; then
    mkdir -p -- "$DATADIR" || die 6 "cannot create datadir: $DATADIR"
    info "created datadir: $DATADIR"
fi

# ------------------------------------------------------- environment checks
# A running daemon owns this datadir. The previous implementation killed it with
# -9 and then deleted chain files underneath it, which risks a corrupted chain.
if command -v pgrep >/dev/null 2>&1 && pgrep -x innovad >/dev/null 2>&1; then
    die 3 "innovad is running; stop it first (e.g. 'innovad stop' or systemctl stop innova)"
fi
if [ -f "$DATADIR/innovad.pid" ]; then
    PID=$(cat -- "$DATADIR/innovad.pid" 2>/dev/null || true)
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        die 3 "datadir pid file $DATADIR/innovad.pid is live (pid $PID); stop innovad first"
    fi
fi

archive_ext() {
    case $1 in
        *.tar.gz|*.tgz) echo tar.gz ;;
        *.tar.zst|*.tzst) echo tar.zst ;;
        *.tar.xz|*.txz) echo tar.xz ;;
        *.tar.bz2|*.tbz2) echo tar.bz2 ;;
        *.tar) echo tar ;;
        *.zip) echo zip ;;
        *.rar) echo rar ;;
        *.7z) echo 7z ;;
        *) echo "" ;;
    esac
}

extractor_for() {
    case $1 in
        zip)   command -v unzip 2>/dev/null || command -v bsdtar 2>/dev/null || true ;;
        rar)   command -v unrar 2>/dev/null || command -v bsdtar 2>/dev/null || true ;;
        7z)    command -v 7z 2>/dev/null || command -v 7za 2>/dev/null || command -v bsdtar 2>/dev/null || true ;;
        tar|tar.*) command -v tar 2>/dev/null || command -v bsdtar 2>/dev/null || true ;;
        *)     true ;;
    esac
}

need_extractor_hint() {
    if [ "$1" = zip ]; then
        printf 'unzip (or bsdtar)'
    else
        printf 'unrar, 7z or bsdtar'
    fi
}

if [ -n "$ARCHIVE" ]; then
    [ -f "$ARCHIVE" ] || die 1 "archive not found: $ARCHIVE"
    EXT=$(archive_ext "$ARCHIVE")
    [ -n "$EXT" ] || die 5 "unsupported archive type: $ARCHIVE (use .zip, .rar, .7z or .tar.*)"
    EXTRACTOR=$(extractor_for "$EXT")
    [ -n "${EXTRACTOR:-}" ] || die 2 "no extractor for .$EXT archives; install $(need_extractor_hint "$EXT")"
else
    EXT=$(archive_ext "$URL")
    [ -n "$EXT" ] || die 5 "cannot infer archive type from URL: $URL"
    EXTRACTOR=$(extractor_for "$EXT")
    [ -n "${EXTRACTOR:-}" ] || die 2 "no extractor for .$EXT archives; install $(need_extractor_hint "$EXT")"
    if command -v curl >/dev/null 2>&1; then
        DOWNLOADER=curl
    elif command -v wget >/dev/null 2>&1; then
        DOWNLOADER=wget
    else
        die 2 "neither curl nor wget is available; install one or use --archive FILE"
    fi
fi

# ------------------------------------------------------------- re-run guard
MARKER=$DATADIR/.bootstrap-installed
if [ -f "$MARKER" ] && [ "$FORCE" -eq 0 ]; then
    info "bootstrap already recorded in $MARKER:"
    sed 's/^/  /' "$MARKER"
    info "re-run with --force to install again"
    exit 0
fi

# ---------------------------------------------------------------- workspace
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/innova-bootstrap.XXXXXX") || die 6 "cannot create a temporary directory"
cleanup() {
    rm -rf -- "$WORKDIR"
}
trap cleanup EXIT HUP INT TERM

# ----------------------------------------------------------------- download
SRC_ARCHIVE=$ARCHIVE
if [ -z "$SRC_ARCHIVE" ]; then
    SRC_ARCHIVE=$WORKDIR/$(basename -- "$URL")
    info "downloading bootstrap: $URL"
    if [ "$DOWNLOADER" = curl ]; then
        curl -fL --retry 3 --retry-delay 2 -o "$SRC_ARCHIVE" "$URL" \
            || die 4 "download failed: $URL"
    else
        wget -O "$SRC_ARCHIVE" "$URL" || die 4 "download failed: $URL"
    fi
fi

[ -s "$SRC_ARCHIVE" ] || die 5 "archive is empty: $SRC_ARCHIVE"

# ------------------------------------------------------------------- verify
ACTUAL_SHA=""
if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL_SHA=$(sha256sum "$SRC_ARCHIVE" | awk '{print $1}')
fi
if [ -n "$WANT_SHA256" ]; then
    [ -n "$ACTUAL_SHA" ] || die 5 "--sha256 requested but sha256sum is unavailable"
    [ "$ACTUAL_SHA" = "$WANT_SHA256" ] || die 5 "sha256 mismatch: expected $WANT_SHA256, got $ACTUAL_SHA"
    info "sha256 verified: $ACTUAL_SHA"
fi

# ------------------------------------------------------------------ extract
EXTRACT_DIR=$WORKDIR/extract
mkdir -p -- "$EXTRACT_DIR"
info "extracting $(basename -- "$SRC_ARCHIVE")"
case $EXT in
    zip)
        case $EXTRACTOR in
            *unzip) unzip -q -o "$SRC_ARCHIVE" -d "$EXTRACT_DIR" || die 5 "unzip failed" ;;
            *)      bsdtar -xf "$SRC_ARCHIVE" -C "$EXTRACT_DIR" || die 5 "bsdtar extraction failed" ;;
        esac
        ;;
    rar)
        case $EXTRACTOR in
            *unrar) unrar x -o+ -idq "$SRC_ARCHIVE" "$EXTRACT_DIR/" || die 5 "unrar failed" ;;
            *)      bsdtar -xf "$SRC_ARCHIVE" -C "$EXTRACT_DIR" || die 5 "bsdtar extraction failed" ;;
        esac
        ;;
    7z)
        case $EXTRACTOR in
            *7z*)  "$EXTRACTOR" x -y -o"$EXTRACT_DIR" "$SRC_ARCHIVE" >/dev/null || die 5 "7z extraction failed" ;;
            *)     bsdtar -xf "$SRC_ARCHIVE" -C "$EXTRACT_DIR" || die 5 "bsdtar extraction failed" ;;
        esac
        ;;
    tar.gz)  tar -xzf "$SRC_ARCHIVE" -C "$EXTRACT_DIR" || die 5 "tar extraction failed" ;;
    tar.zst) tar --zstd -xf "$SRC_ARCHIVE" -C "$EXTRACT_DIR" || die 5 "tar --zstd extraction failed" ;;
    tar.xz)  tar -xJf "$SRC_ARCHIVE" -C "$EXTRACT_DIR" || die 5 "tar extraction failed" ;;
    tar.bz2) tar -xjf "$SRC_ARCHIVE" -C "$EXTRACT_DIR" || die 5 "tar extraction failed" ;;
    tar)     tar -xf  "$SRC_ARCHIVE" -C "$EXTRACT_DIR" || die 5 "tar extraction failed" ;;
esac

# Locate the directory that actually holds the chain data: the directory of the
# first blk*.dat found anywhere in the extracted tree (archives wrap the payload
# in one or more container directories).
CHAIN_SRC=""
FIRST_BLK=$(find "$EXTRACT_DIR" -type f -name 'blk*.dat' 2>/dev/null | head -n 1)
[ -n "$FIRST_BLK" ] && CHAIN_SRC=$(dirname -- "$FIRST_BLK")
[ -n "$CHAIN_SRC" ] || die 5 "the archive does not contain chain data (no blk*.dat found)"
info "chain data found in: $(basename -- "$CHAIN_SRC")"

# ------------------------------------------------------------------ install
TS=$(date -u +%Y%m%dT%H%M%SZ)
BACKUP_DIR=$DATADIR/bootstrap-backup-$TS
[ -e "$BACKUP_DIR" ] && BACKUP_DIR=$BACKUP_DIR-$$
MOVED=0
move_aside() {
    [ "$MOVED" -eq 0 ] && mkdir -p -- "$BACKUP_DIR" && MOVED=1
    mv -- "$1" "$BACKUP_DIR/" || die 6 "cannot move $1 aside"
    info "backed up: $(basename -- "$1") -> $(basename -- "$BACKUP_DIR")/"
}
for item in $CHAIN_ITEMS; do
    [ -e "$DATADIR/$item" ] && move_aside "$DATADIR/$item"
done
for f in "$DATADIR"/blk[0-9][0-9][0-9][0-9].dat; do
    [ -e "$f" ] || continue
    move_aside "$f"
done

# Copy chain data, never the wallet or configuration.
COPY_FAILED=0
copy_item() {
    rel=$1
    case $rel in
        ./wallet.dat|./innova.conf|./masternode.conf|*.lock|./.lock) return 0 ;;
    esac
    dest=$DATADIR/${rel#./}
    mkdir -p -- "$(dirname -- "$dest")" || return 1
    cp -f -- "$CHAIN_SRC/$rel" "$dest" || return 1
}
( cd "$CHAIN_SRC" && find . -type f ) > "$WORKDIR/filelist" || die 6 "cannot enumerate archive contents"
while IFS= read -r rel; do
    copy_item "$rel" || COPY_FAILED=1
done < "$WORKDIR/filelist"
[ "$COPY_FAILED" -eq 0 ] || die 6 "copying chain data into $DATADIR failed"

if [ "$KEEP_ARCHIVE" -eq 1 ]; then
    cp -f -- "$SRC_ARCHIVE" "$DATADIR/$(basename -- "$SRC_ARCHIVE")" || die 6 "cannot keep archive"
    info "archive kept: $DATADIR/$(basename -- "$SRC_ARCHIVE")"
fi

if [ -n "$ARCHIVE" ]; then
    SOURCE_DESC="local:$ARCHIVE"
else
    SOURCE_DESC=$URL
fi
if [ "$MOVED" -eq 1 ]; then
    BACKUP_DESC=$BACKUP_DIR
else
    BACKUP_DESC=none
fi
{
    printf 'installed_utc=%s\n' "$TS"
    printf 'source=%s\n' "$SOURCE_DESC"
    printf 'sha256=%s\n' "${ACTUAL_SHA:-unavailable}"
    printf 'datadir=%s\n' "$DATADIR"
    printf 'previous_chain_backup=%s\n' "$BACKUP_DESC"
} > "$MARKER" || die 6 "cannot write install marker: $MARKER"

info "bootstrap installed into: $DATADIR"
[ "$MOVED" -eq 1 ] && info "previous chain files preserved in: $BACKUP_DIR"

# -------------------------------------------------------------------- start
if [ "$START" -eq 1 ]; then
    if [ -x "$PROJECT_ROOT/src/innovad" ]; then
        INNOVAD=$PROJECT_ROOT/src/innovad
    elif command -v innovad >/dev/null 2>&1; then
        INNOVAD=$(command -v innovad)
    else
        die 2 "--start requested but no innovad binary found (build src/innovad or install it)"
    fi
    info "starting: $INNOVAD -datadir=$DATADIR -daemon"
    "$INNOVAD" -datadir="$DATADIR" -daemon || die 6 "innovad failed to start"
    info "innovad started; check progress with: $INNOVAD -datadir=$DATADIR getinfo"
else
    info "start the node when ready, e.g.: innovad -datadir=$DATADIR -daemon"
fi

info "bootstrap completed"
