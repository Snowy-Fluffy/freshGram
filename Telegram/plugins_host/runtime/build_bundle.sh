#!/bin/bash
# Builds the linux-x86_64 plugin runtime bundle on Rocky Linux 8 (glibc 2.28) in Docker.
set -euo pipefail

WASMTIME_VERSION=50.0.0
WASMTIME_SHA256=631643d8be5dac2a1abe026528f0ca18a515bce275bcfa7eb9731c9468a592f1
PYTHON_VERSION=3.14.7
PYTHON_ARCHIVE=python-${PYTHON_VERSION}-wasi_sdk-24.zip
PYTHON_SHA256=2e064d3fb8172471d39d741348efa722349c40b96301f69968dff714999c584b
HOST_VERSION=1
GLIBC_FLOOR=2.28
IMAGE=freshgram-plugin-host-builder

usage() {
	echo "usage: $0 <revision> [--min-revision N] [--out DIR] [--skip-tests]" >&2
	exit 2
}

[[ $# -ge 1 && $1 =~ ^[0-9]+$ ]] || usage
revision=$1
shift
min_revision=1
out=$HOME/freshgram-runtime
tests=1
while [[ $# -gt 0 ]]; do
	case $1 in
		--min-revision) min_revision=${2:?}; shift 2 ;;
		--out) out=${2:?}; shift 2 ;;
		--skip-tests) tests=0; shift ;;
		*) usage ;;
	esac
done
[[ $min_revision =~ ^[0-9]+$ && $min_revision -le $revision ]] || usage

here=$(cd "$(dirname "$0")" && pwd)
host_dir=$(dirname "$here")
telegram=$(dirname "$host_dir")
third_party=$host_dir/third_party
build=$host_dir/build-bundle
target=$out/rev-$revision

if [[ -e $target ]]; then
	echo "$target already exists, refusing to overwrite it" >&2
	exit 1
fi

fetch() {
	local url=$1 file=$2 sha=$3
	if [[ ! -f $file ]]; then
		echo "downloading $url"
		curl -fL --retry 3 -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	if ! echo "$sha  $file" | sha256sum -c --quiet -; then
		echo "$file does not match the pinned SHA-256" >&2
		exit 1
	fi
}

mkdir -p "$third_party/cpython-wasi"
wasmtime_name=wasmtime-v$WASMTIME_VERSION-x86_64-linux-c-api
fetch "https://github.com/bytecodealliance/wasmtime/releases/download/v$WASMTIME_VERSION/$wasmtime_name.tar.xz" \
	"$third_party/$wasmtime_name.tar.xz" "$WASMTIME_SHA256"
if [[ ! -d $third_party/$wasmtime_name ]]; then
	tar -xJf "$third_party/$wasmtime_name.tar.xz" -C "$third_party"
fi
fetch "https://github.com/brettcannon/cpython-wasi-build/releases/download/v$PYTHON_VERSION/$PYTHON_ARCHIVE" \
	"$third_party/cpython-wasi/$PYTHON_ARCHIVE" "$PYTHON_SHA256"
if [[ ! -f $third_party/cpython-wasi/python.wasm ]]; then
	unzip -q -o "$third_party/cpython-wasi/$PYTHON_ARCHIVE" -d "$third_party/cpython-wasi"
fi

docker build -q -t "$IMAGE" "$here" >/dev/null
mkdir -p "$build"
in_docker() {
	docker run --rm --security-opt seccomp=unconfined -u "$(id -u):$(id -g)" \
		-v "$telegram:/t:ro" -v "$build:/out" "$@"
}
echo "building the host, log: $build/build.log"
if ! in_docker "$IMAGE" bash -ec "
		source /opt/rh/gcc-toolset-13/enable
		cmake -S /t/plugins_host -B /out -DCMAKE_BUILD_TYPE=Release \
			-DFGPL_WASMTIME_DIR=/t/plugins_host/third_party/$wasmtime_name
		cmake --build /out -j\$(nproc)
		strip -o /out/fgpl_plugin_host.stripped /out/fgpl_plugin_host" >"$build/build.log" 2>&1; then
	tail -30 "$build/build.log" >&2
	exit 1
fi
if [[ $tests == 1 ]]; then
	in_docker "$IMAGE" ctest --test-dir /out --output-on-failure -j4 | tail -3
fi

host=$build/fgpl_plugin_host.stripped
needed=$(readelf -d "$host" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
for library in $needed; do
	case $library in
		libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|ld-linux-x86-64.so.2) ;;
		*) echo "the host needs $library, which is not part of glibc" >&2; exit 1 ;;
	esac
done
glibc=$(objdump -T "$host" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
if [[ $(printf '%s\n%s\n' "$glibc" "$GLIBC_FLOOR" | sort -V | tail -1) != "$GLIBC_FLOOR" ]]; then
	echo "the host needs glibc $glibc, newer than $GLIBC_FLOOR" >&2
	exit 1
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/python" "$stage/sdk"
install -m 755 "$host" "$stage/fgpl_plugin_host"
cp "$third_party/cpython-wasi/python.wasm" "$stage/python/"
cp -r "$third_party/cpython-wasi/lib" "$stage/python/lib"
cp -r "$telegram/plugins_sdk/python/freshgram" "$stage/sdk/freshgram"
find "$stage" -name __pycache__ -prune -exec rm -rf {} +
find "$stage" -name '*.pyc' -delete

mkdir -p "$target"
bundle=$target/runtime-linux-x86_64.zip
chmod 755 "$stage"
in_docker -v "$stage:/stage:ro" -v "$target:/target" "$IMAGE" \
	/out/fgpl_tool pack-runtime /stage /target/runtime-linux-x86_64.zip >/dev/null
sha=sha256-$(sha256sum "$bundle" | cut -d' ' -f1 | xxd -r -p | base64)
size=$(stat -c %s "$bundle")

cat > "$target/runtime.unsigned.json" <<EOF
{"schema":1,"revision":$revision,"minRevision":$min_revision,"host":$HOST_VERSION,"wasmtime":"$WASMTIME_VERSION","python":"$PYTHON_VERSION","revoked":[],"bundles":{"linux-x86_64":{"post":POST_ID,"sha256":"$sha","size":$size}}}
EOF

cat <<EOF

Bundle:  $bundle
Host:    glibc $glibc+, libraries: $(echo $needed)
Index:   $target/runtime.unsigned.json

Next:
  1. Post $(basename "$bundle") to @freshgram_runtime as a file and note the post id N.
  2. sed -i 's/POST_ID/N/' $target/runtime.unsigned.json
  3. Sign it, for example with the tool built here:
     docker run --rm -u \$(id -u):\$(id -g) -v $build:/out:ro -v $target:/target \\
       -v <runtime.key>:/runtime.key:ro $IMAGE \\
       /out/fgpl_tool sign-runtime /target/runtime.unsigned.json /runtime.key /target/runtime.json
  4. Post runtime.json to the channel as a file and pin that post.
EOF
