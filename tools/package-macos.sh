#!/usr/bin/env bash
# Assemble a relocatable macOS runtime directory for qemu-system-mipsel.
#
# Usage: tools/package-macos.sh <qemu-binary> <output-directory>
#
# Homebrew dylibs carry absolute install names, so copying them is not enough:
# every reference is rewritten to @loader_path/@executable_path and every
# Mach-O file is re-signed ad-hoc. The result needs no Homebrew installation.
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 <qemu-binary> <output-directory>" >&2
    exit 2
fi
src_bin=$1
out_dir=$2
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
src_bin=$(cd "$(dirname "$src_bin")" && pwd)/$(basename "$src_bin")

if [ ! -x "$src_bin" ]; then
    echo "not an executable: $src_bin" >&2
    exit 1
fi

brew_prefix=${BREW_PREFIX:-$(brew --prefix)}

rm -rf "$out_dir"
mkdir -p "$out_dir/bin" "$out_dir/lib" "$out_dir/share"
install -m 0755 "$src_bin" "$out_dir/bin/qemu-system-mipsel"

is_system_dep() {
    case $1 in
        /usr/lib/*|/System/Library/*|/Library/Apple/*) return 0 ;;
    esac
    return 1
}

# Plain-text set: macOS ships bash 3.2, which has no associative arrays.
bundled_list=$(mktemp -t book2-bundle)
trap 'rm -f "$bundled_list"' EXIT
is_bundled() { grep -Fxq "$1" "$bundled_list"; }
add_bundled() { printf '%s\n' "$1" >> "$bundled_list"; }

# Resolve one LC_LOAD_DYLIB entry to a real file on the build machine.
resolve_dependency() {
    local obj=$1 dep=$2 rel candidate
    case $dep in
        @loader_path/*)
            rel=${dep#@loader_path/}
            printf '%s\n' "$(dirname "$obj")/$rel"
            ;;
        @executable_path/*)
            rel=${dep#@executable_path/}
            printf '%s\n' "$out_dir/bin/$rel"
            ;;
        @rpath/*)
            rel=${dep#@rpath/}
            for candidate in \
                "$(dirname "$obj")/$rel" \
                "$brew_prefix/lib/$rel" \
                /opt/homebrew/lib/"$rel" \
                /usr/local/lib/"$rel"; do
                if [ -e "$candidate" ]; then
                    printf '%s\n' "$candidate"
                    return 0
                fi
            done
            for candidate in "$brew_prefix"/opt/*/lib/"$rel"; do
                if [ -e "$candidate" ]; then
                    printf '%s\n' "$candidate"
                    return 0
                fi
            done
            return 1
            ;;
        /*)
            printf '%s\n' "$dep"
            ;;
    esac
    return 1
}

collect_from() {
    while [ ${#queue[@]} -gt 0 ]; do
        obj=${queue[0]}
        queue=("${queue[@]:1}")
        while IFS= read -r dep; do
            [ -n "$dep" ] || continue
            is_system_dep "$dep" && continue
            [ "$dep" = "$obj" ] && continue
            if [ -e "$dep" ] && [ "$dep" -ef "$obj" ]; then
                continue
            fi
            base=$(basename "$dep")
            is_bundled "$base" && continue
            dependency=$(resolve_dependency "$obj" "$dep" || true)
            if [ -z "$dependency" ] || [ ! -e "$dependency" ]; then
                echo "cannot resolve $dep (needed by $obj)" >&2
                exit 1
            fi
            cp -L "$dependency" "$out_dir/lib/$base"
            add_bundled "$base"
            queue+=("$out_dir/lib/$base")
        done < <(otool -L "$obj" | awk 'NR > 1 { print $1 }')
    done
}

queue=("$out_dir/bin/qemu-system-mipsel")
collect_from

# GdkPixbuf loaders live three levels below lib/, so their relative search
# path differs from plain dylibs.
pixbuf_loaders=$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0 2>/dev/null || true)
loaders_rel=lib/gdk-pixbuf-2.0/2.10.0/loaders
if [ -n "$pixbuf_loaders" ] && [ -d "$pixbuf_loaders" ]; then
    mkdir -p "$out_dir/$loaders_rel"
    cp -L "$pixbuf_loaders"/*.so "$out_dir/$loaders_rel/" 2>/dev/null || true
    for loader in "$out_dir/$loaders_rel"/*.so; do
        [ -e "$loader" ] || continue
        queue+=("$loader")
    done
    collect_from

    query_loaders=$(command -v gdk-pixbuf-query-loaders 2>/dev/null || true)
    if [ -n "$query_loaders" ]; then
        GDK_PIXBUF_MODULEDIR="$out_dir/$loaders_rel" \
            "$query_loaders" > "$out_dir/share/loaders.cache"
        python3 - "$out_dir" "$out_dir/$loaders_rel" <<'PY'
import pathlib, sys
out = pathlib.Path(sys.argv[1])
module_dir = pathlib.Path(sys.argv[2])
cache = out / "share" / "loaders.cache"
cache.write_text(cache.read_text().replace(
    str(module_dir), "@HERE@/lib/gdk-pixbuf-2.0/2.10.0/loaders"))
PY
    fi
fi

# Rewrite every dylib reference to a relative one.
for obj in "$out_dir/bin/qemu-system-mipsel" $(find "$out_dir/lib" -type f \( -name '*.dylib' -o -name '*.so' \)); do
    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        is_system_dep "$dep" && continue
        [ "$dep" = "$obj" ] && continue
        base=$(basename "$dep")
        if ! is_bundled "$base"; then
            echo "unbundled reference $dep in $obj" >&2
            exit 1
        fi
        case $obj in
            "$out_dir/bin/"*) prefix='@executable_path/../lib/' ;;
            "$out_dir/lib/gdk-pixbuf-2.0/"*) prefix='@loader_path/../../../' ;;
            *) prefix='@loader_path/' ;;
        esac
        install_name_tool -change "$dep" "$prefix$base" "$obj"
    done < <(otool -L "$obj" | awk 'NR > 1 { print $1 }')
done

for lib in "$out_dir/lib"/*.dylib; do
    [ -e "$lib" ] || continue
    install_name_tool -id "@rpath/$(basename "$lib")" "$lib"
done

# Homebrew data files the GTK stack expects next to its libraries.
copy_tree() {
    local source=$1 relative=$2
    if [ -d "$source" ]; then
        mkdir -p "$out_dir/$relative"
        cp -R "$source/." "$out_dir/$relative/"
    fi
}

glib_prefix=$(pkg-config --variable=prefix glib-2.0 2>/dev/null || true)
gtk_prefix=$(pkg-config --variable=prefix gtk+-3.0 2>/dev/null || true)
mime_prefix=$(brew --prefix shared-mime-info 2>/dev/null || true)
adwaita_prefix=$(brew --prefix adwaita-icon-theme 2>/dev/null || true)

copy_tree "${glib_prefix:-$brew_prefix}/share/glib-2.0/schemas" "share/glib-2.0/schemas"
# GTK ships its own GSettings schemas in its keg, not in glib's.
copy_tree "${gtk_prefix:-$brew_prefix}/share/glib-2.0/schemas" "share/glib-2.0/schemas"
copy_tree "${mime_prefix:-$brew_prefix}/share/mime" "share/mime"
copy_tree "${gtk_prefix:-$brew_prefix}/share/gtk-3.0" "share/gtk-3.0"
copy_tree "${gtk_prefix:-$brew_prefix}/etc/gtk-3.0" "etc/gtk-3.0"
copy_tree "${adwaita_prefix:-$brew_prefix}/share/icons/hicolor" "share/icons/hicolor"
copy_tree "${adwaita_prefix:-$brew_prefix}/share/icons/Adwaita" "share/icons/Adwaita"
glib_schemas_tool=$(command -v glib-compile-schemas 2>/dev/null || true)
[ -n "$glib_schemas_tool" ] || glib_schemas_tool="${glib_prefix:-$brew_prefix}/bin/glib-compile-schemas"
"$glib_schemas_tool" "$out_dir/share/glib-2.0/schemas"

if [ -d "$repo_dir/pc-bios/keymaps" ]; then
    mkdir -p "$out_dir/share/qemu"
    cp -R "$repo_dir/pc-bios/keymaps" "$out_dir/share/qemu/"
fi
for icon in "$repo_dir"/ui/icons/qemu_*x*.png; do
    [ -e "$icon" ] || continue
    name=$(basename "$icon")
    size=${name#qemu_}
    size=${size%.png}
    mkdir -p "$out_dir/share/icons/hicolor/$size/apps"
    install -m 0644 "$icon" "$out_dir/share/icons/hicolor/$size/apps/qemu.png"
done
if [ -f "$repo_dir/README.md" ]; then
    install -m 0644 "$repo_dir/README.md" "$out_dir/README.md"
fi

cat > "$out_dir/run.sh" <<'RUN'
#!/bin/sh
HERE=$(cd "$(dirname "$0")" && pwd)
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

# Ad-hoc signing is mandatory after install_name_tool on Apple silicon.
for obj in "$out_dir/bin/qemu-system-mipsel" $(find "$out_dir/lib" -type f \( -name '*.dylib' -o -name '*.so' \)); do
    codesign --force --sign - "$obj"
done

# Refuse to publish a bundle that still points at the build machine.
if otool -L "$out_dir/bin/qemu-system-mipsel" | grep -E '/opt/homebrew|/usr/local/Cellar|/usr/local/opt' >/dev/null; then
    echo "executable still references Homebrew paths:" >&2
    otool -L "$out_dir/bin/qemu-system-mipsel" | grep -E '/opt/homebrew|/usr/local' >&2
    exit 1
fi

echo "packaged $(find "$out_dir" -type f | wc -l) files into $out_dir"
