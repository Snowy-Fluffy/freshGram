#!/usr/bin/env bash
set -euo pipefail

usr="$(realpath "$1")"
bundle="$(realpath -m "$2")"
here="$(dirname "$(realpath "$0")")"

sudo apt-get update
sudo apt-get install -y flatpak flatpak-builder
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y --noninteractive flathub org.gnome.Platform//51 org.gnome.Sdk//51

mkdir -p "$(dirname "$bundle")"
work="$(mktemp -d)"
mkdir -p "$work/root"
cp "$here/io.github.snowyfluffy.freshgram.yml" "$work/"
cp -a "$usr" "$work/root/usr"
cd "$work"
flatpak-builder --user --force-clean --disable-rofiles-fuse --repo=repo build io.github.snowyfluffy.freshgram.yml
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo repo "$bundle" io.github.snowyfluffy.freshgram
