#!/usr/bin/env bash
set -euo pipefail

usr="$(realpath "$1")"
output="$(realpath -m "$2")"
version="${3:?package version is required}"
here="$(dirname "$(realpath "$0")")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/root" "$work/rpm"
cp -a "$usr" "$work/root/usr"
find "$work/root" -type d -exec chmod 755 {} +

machine="$(uname -m)"
rpmbuild -bb \
	--define "_topdir $work/rpm" \
	--define "freshgram_arch $machine" \
	--define "freshgram_version ${version//-/.}" \
	--define "freshgram_root $work/root" \
	"$here/freshgram.spec"

mkdir -p "$(dirname "$output")"
cp "$work"/rpm/RPMS/"$machine"/freshgram-*."$machine".rpm "$output"
