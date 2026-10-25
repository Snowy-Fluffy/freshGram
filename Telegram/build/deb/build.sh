#!/usr/bin/env bash
set -euo pipefail

usr="$(realpath "$1")"
output="$(realpath -m "$2")"
version="${3:?package version is required}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

root="$work/root"
mkdir -p "$root/DEBIAN"
cp -a "$usr" "$root/usr"
find "$root" -type d -exec chmod 755 {} +

size="$(du -sk "$root/usr" | cut -f1)"
case "$(uname -m)" in
aarch64) architecture=arm64 ;;
*) architecture=amd64 ;;
esac
cat > "$root/DEBIAN/control" <<CONTROL
Package: freshgram
Version: $version
Section: net
Priority: optional
Architecture: $architecture
Installed-Size: $size
Maintainer: freshGram <noreply@users.noreply.github.com>
Depends: libc6, libstdc++6, hicolor-icon-theme
Recommends: xdg-desktop-portal
Homepage: https://github.com/Snowy-Fluffy/freshGram
Description: Telegram Desktop fork with the AyuGram features and a Material look
 freshGram is a Telegram Desktop client that keeps deleted and edited
 messages, adds a ghost mode and secret chats, and restyles the interface.
CONTROL

for script in postinst postrm; do
	cat > "$root/DEBIAN/$script" <<'SCRIPT'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
	update-desktop-database -q /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
	gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
exit 0
SCRIPT
	chmod 755 "$root/DEBIAN/$script"
done

mkdir -p "$(dirname "$output")"
dpkg-deb --build --root-owner-group -Zxz "$root" "$output"
