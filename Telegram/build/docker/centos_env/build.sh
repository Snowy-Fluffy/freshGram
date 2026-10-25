#!/bin/bash
set -e

cd Telegram
./configure.sh "$@"

config="${CONFIG:-Release}"

# KEEP_GOING=1 lets ninja report every failing translation unit in one
# run instead of stopping at the first; the exit status still fails.
build() {
	if [[ -n $BUILD_TIMEOUT ]]; then
		timeout --signal=INT --kill-after=5m "$BUILD_TIMEOUT" \
			cmake --build ../out --config "$config" ${KEEP_GOING:+-- -k 0}
	else
		cmake --build ../out --config "$config" ${KEEP_GOING:+-- -k 0}
	fi
}

# WHY: an interrupted ninja removes the outputs it did not finish only if it
# outlives the container, and a poisoned ccache entry comes back on a plain
# rebuild, so broken objects are dropped and rebuilt without reading the cache.
rebuild_broken() {
	local broken
	broken=$(python3 ../.github/scripts/drop_broken_objects.py ../out)
	[[ -n $broken ]] || return 1
	while read -r object; do
		CCACHE_RECACHE=1 cmake --build ../out --config "$config" --target "$object" || true
	done <<< "$broken"
}

wait_for_ninja() {
	local waited=0
	while grep -qsx ninja /proc/[0-9]*/comm && (( waited++ < 300 )); do
		sleep 1
	done
}

rebuild_broken || true
status=0
build || status=$?
if (( status == 124 || status == 137 )); then
	wait_for_ninja
	exit "$status"
elif (( status != 0 )); then
	rebuild_broken || exit "$status"
	build
fi

([[ -d ../out/install ]] && rm -rf ../out/install; mkdir -p ../out/install) && DESTDIR=../out/install cmake --install ../out --config "$config"
