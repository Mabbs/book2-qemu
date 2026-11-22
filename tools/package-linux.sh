#!/usr/bin/env bash
# Assemble a relocatable Linux runtime directory for qemu-system-mipsel.
#
# Usage: tools/package-linux.sh <qemu-binary> <output-directory>
#
# The result contains the emulator, every non-glibc shared object it
# transitively needs, the GLib/GTK runtime data files and a run.sh wrapper.
# Build it inside the oldest distribution you intend to support: the emulator
# still uses the build host's glibc and ELF interpreter.
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 <qemu-binary> <output-directory>" >&2
    exit 2
fi
src_bin=$1
out_dir=$2
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
src_bin=$(CDPATH='' cd -- "$(dirname -- "$src_bin")" && pwd)/$(basename -- "$src_bin")
case $out_dir in
    /*) ;;
    *) out_dir=$(pwd)/$out_dir ;;
esac
if [ "$out_dir" = "/" ]; then
    echo "refusing to package into /" >&2
    exit 1
fi

if [ ! -x "$src_bin" ]; then
    echo "not an executable: $src_bin" >&2
    exit 1
fi

rm -rf -- "$out_dir"
mkdir -p -- "$out_dir/bin" "$out_dir/lib" "$out_dir/share/qemu"
install -m 0755 -- "$src_bin" "$out_dir/bin/qemu-system-mipsel"

# Libraries supplied by glibc/the dynamic loader itself. Everything else,
# including libgcc_s and the C++ runtime, is shipped with the bundle.
is_host_library() {
    case $(basename -- "$1") in
        libc.so.*|libm.so.*|libdl.so.*|librt.so.*|libpthread.so.*|libresolv.so.*|libutil.so.*|libanl.so.*)
            return 0 ;;
        ld-linux*|ld64.so.*|libnss_*.so.*|libthread_db.so.*)
            return 0 ;;
    esac
    return 1
}

declare -A copied=()
queue=("$out_dir/bin/qemu-system-mipsel")

while [ ${#queue[@]} -gt 0 ]; do
    obj=${queue[0]}
    queue=("${queue[@]:1}")

    if ldd "$obj" 2>/dev/null | grep -q 'not found'; then
        echo "unresolved dependency while packaging $obj:" >&2
        ldd "$obj" | grep 'not found' >&2
        exit 1
    fi

    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        base=$(basename -- "$dep")
        if is_host_library "$base"; then
            continue
        fi
        if [ -n "${copied[$base]:-}" ]; then
            continue
        fi
        if [ ! -e "$dep" ]; then
            echo "dependency disappeared: $dep" >&2
            exit 1
        fi
        copied[$base]=1
        cp -L -- "$dep" "$out_dir/lib/$base"
        queue+=("$out_dir/lib/$base")
    done < <(ldd "$obj" 2>/dev/null | awk '
        /=>/ && $3 ~ /^\// { print $3 }
        /^[[:space:]]*\/.*\(0x/ { print $1 }
    ')
done

# Optional image loaders (PNG/BMP icons, file chooser thumbnails). They live
# one directory deeper than the libraries, hence the different $ORIGIN path.
pixbuf_loaders=$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0 2>/dev/null || true)
if [ -n "$pixbuf_loaders" ] && [ -d "$pixbuf_loaders" ]; then
    loaders_rel=lib/gdk-pixbuf-2.0/2.10.0/loaders
    mkdir -p "$out_dir/$loaders_rel"
    cp -L -- "$pixbuf_loaders"/*.so "$out_dir/$loaders_rel/" 2>/dev/null || true
    for loader in "$out_dir/$loaders_rel"/*.so; do
        [ -e "$loader" ] || continue
        queue+=("$loader")
    done
    while [ ${#queue[@]} -gt 0 ]; do
        obj=${queue[0]}
        queue=("${queue[@]:1}")
        while IFS= read -r dep; do
            [ -n "$dep" ] || continue
            base=$(basename -- "$dep")
            is_host_library "$base" && continue
            [ -n "${copied[$base]:-}" ] && continue
            copied[$base]=1
            cp -L -- "$dep" "$out_dir/lib/$base"
            queue+=("$out_dir/lib/$base")
        done < <(ldd "$obj" 2>/dev/null | awk '
            /=>/ && $3 ~ /^\// { print $3 }
            /^[[:space:]]*\/.*\(0x/ { print $1 }
        ')
    done
    query_loaders=$(command -v gdk-pixbuf-query-loaders 2>/dev/null || true)
    if [ -z "$query_loaders" ]; then
        # Debian/Ubuntu keep the tool next to (not inside) the module dir.
        candidate=$(dirname -- "$(dirname -- "$pixbuf_loaders")")/gdk-pixbuf-query-loaders
        [ -x "$candidate" ] && query_loaders=$candidate
    fi
    if [ -n "$query_loaders" ]; then
        GDK_PIXBUF_MODULEDIR="$out_dir/$loaders_rel" \
            "$query_loaders" > "$out_dir/share/loaders.cache"
        python3 - "$out_dir" "$out_dir/$loaders_rel" <<'PY'
import pathlib, sys
out = pathlib.Path(sys.argv[1])
module_dir = pathlib.Path(sys.argv[2])
cache = out / "share" / "loaders.cache"
text = cache.read_text()
cache.write_text(text.replace(str(module_dir), "@HERE@/lib/gdk-pixbuf-2.0/2.10.0/loaders"))
PY
    else
        rm -f "$out_dir/share/loaders.cache"
    fi
fi

# GLib settings schemas are mandatory: g_settings_new() aborts when the
# schema is absent, which is exactly what happens on a machine without GTK.
mkdir -p "$out_dir/share/glib-2.0/schemas"
for schema in /usr/share/glib-2.0/schemas/*; do
    [ -e "$schema" ] || continue
    cp -a -- "$schema" "$out_dir/share/glib-2.0/schemas/"
done
glib-compile-schemas "$out_dir/share/glib-2.0/schemas"

# GdkPixbuf identifies image formats through the shared-mime-info database.
# Without it GTK cannot even load its own built-in "image-missing" fallback.
for data_dir in /usr/share/gtk-3.0 /usr/share/mime /usr/share/icons/hicolor /usr/share/icons/Adwaita; do
    [ -d "$data_dir" ] || continue
    mkdir -p "$out_dir/share/$(dirname -- "${data_dir#/usr/share/}")"
    cp -a -- "$data_dir/." "$out_dir/share/${data_dir#/usr/share/}/"
done

# QEMU looks up keymaps and a few firmware blobs in <datadir>/qemu. The Book2
# machine needs no firmware, but VNC/SDL keymaps still come from here.
if [ -d "$repo_dir/pc-bios/keymaps" ]; then
    cp -a -- "$repo_dir/pc-bios/keymaps" "$out_dir/share/qemu/"
fi

# Window icon: gtk_window_set_icon_name("qemu") searches the hicolor theme.
for icon in "$repo_dir"/ui/icons/qemu_*x*.png; do
    [ -e "$icon" ] || continue
    name=$(basename -- "$icon")
    size=${name#qemu_}
    size=${size%.png}
    install -D -m 0644 -- "$icon" "$out_dir/share/icons/hicolor/$size/apps/qemu.png"
done

if [ -f "$repo_dir/README.md" ]; then
    install -m 0644 -- "$repo_dir/README.md" "$out_dir/README.md"
fi

# Rewrite the search path instead of relying on LD_LIBRARY_PATH, so the binary
# also works when invoked directly. $ORIGIN keeps everything relocatable.
find "$out_dir/lib" -type f -name '*.so*' -print0 | while IFS= read -r -d '' lib; do
    case $lib in
        */gdk-pixbuf-2.0/*/loaders/*) rpath='$ORIGIN/../../..' ;;
        *) rpath='$ORIGIN' ;;
    esac
    patchelf --set-rpath "$rpath" "$lib"
done
patchelf --set-rpath '$ORIGIN/../lib' "$out_dir/bin/qemu-system-mipsel"

cat > "$out_dir/run.sh" <<'RUN'
#!/bin/sh
HERE=$(CDPATH= cd "$(dirname "$0")" && pwd)
LD_LIBRARY_PATH="$HERE/lib"
export LD_LIBRARY_PATH
XDG_DATA_DIRS="$HERE/share:/usr/local/share:/usr/share"
export XDG_DATA_DIRS
GSETTINGS_SCHEMA_DIR="$HERE/share/glib-2.0/schemas"
export GSETTINGS_SCHEMA_DIR
if [ -f "$HERE/share/loaders.cache" ]; then
    CACHE="${TMPDIR:-/tmp}/book2-gdk-pixbuf-$$.cache"
    sed "s|@HERE@|$HERE|g" "$HERE/share/loaders.cache" > "$CACHE"
    GDK_PIXBUF_MODULE_FILE="$CACHE"
    export GDK_PIXBUF_MODULE_FILE
fi
"$HERE/bin/qemu-system-mipsel" -L "$HERE/share/qemu" "$@"
status=$?
[ -n "${CACHE:-}" ] && rm -f "$CACHE"
exit $status
RUN
chmod 0755 "$out_dir/run.sh"

# Fail the build if the bundle still reaches back into the build machine.
if LD_LIBRARY_PATH="$out_dir/lib" ldd "$out_dir/bin/qemu-system-mipsel" 2>/dev/null | grep -q 'not found'; then
    echo "bundle is incomplete:" >&2
    LD_LIBRARY_PATH="$out_dir/lib" ldd "$out_dir/bin/qemu-system-mipsel" | grep 'not found' >&2
    exit 1
fi
while IFS= read -r dep; do
    [ -n "$dep" ] || continue
    case $dep in
        "$out_dir"/*) ;;
        *)
            if ! is_host_library "$(basename -- "$dep")"; then
                echo "bundle still uses host library: $dep" >&2
                exit 1
            fi
            ;;
    esac
done < <(LD_LIBRARY_PATH="$out_dir/lib" ldd "$out_dir/bin/qemu-system-mipsel" 2>/dev/null | awk '
    /=>/ && $3 ~ /^\// { print $3 }
    /^[[:space:]]*\/.*\(0x/ { print $1 }
')

echo "packaged $(find "$out_dir" -type f | wc -l) files into $out_dir"
