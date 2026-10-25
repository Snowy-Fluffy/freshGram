#!/usr/bin/env bash
set -euo pipefail

usr="$(realpath "$1")"
output="$(realpath -m "$2")"

machine="$(uname -m)"
case "$machine" in
x86_64)
	tool_sha256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
	runtime_sha256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
	;;
aarch64)
	tool_sha256=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
	runtime_sha256=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444
	;;
*)
	echo "No AppImage tools for $machine" >&2
	exit 1
	;;
esac
tool_url=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-$machine.AppImage
runtime_url=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-$machine
app_id=io.github.snowyfluffy.freshgram

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fetch() {
	curl -fsSL --retry 3 -o "$1" "$2"
	echo "$3  $1" | sha256sum -c -
}

fetch "$work/appimagetool" "$tool_url" "$tool_sha256"
fetch "$work/runtime" "$runtime_url" "$runtime_sha256"
chmod +x "$work/appimagetool"

appdir="$work/AppDir"
mkdir -p "$appdir"
cp -a "$usr" "$appdir/usr"
rm -rf "$appdir/usr/share/dbus-1"

sed \
	-e 's|^Exec=env DESKTOPINTEGRATION=1 freshGram|Exec=freshGram|' \
	-e '/^TryExec=/d' \
	-e '/^DBusActivatable=/d' \
	"$appdir/usr/share/applications/$app_id.desktop" > "$appdir/$app_id.desktop"
cp "$appdir/usr/share/icons/hicolor/256x256/apps/$app_id.png" "$appdir/$app_id.png"
cp "$appdir/$app_id.png" "$appdir/.DirIcon"

cat > "$appdir/AppRun" <<'RUN'
#!/bin/sh
here="$(dirname "$(readlink -f "$0")")"
export PATH="$here/usr/bin:$PATH"
export XDG_DATA_DIRS="$here/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$here/usr/bin/freshGram" "$@"
RUN
chmod +x "$appdir/AppRun"

mkdir -p "$(dirname "$output")"
ARCH="$machine" "$work/appimagetool" --appimage-extract-and-run \
	--no-appstream \
	--runtime-file "$work/runtime" \
	"$appdir" "$output"
chmod +x "$output"
