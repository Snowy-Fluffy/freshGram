#!/bin/bash
# Builds the plugin runtime bundles on Rocky Linux 8 (glibc 2.28) in Docker: x86_64 natively, aarch64 with clang.
set -euo pipefail

WASMTIME_VERSION=50.0.0
declare -A WASMTIME_SHA256=(
	[x86_64]=631643d8be5dac2a1abe026528f0ca18a515bce275bcfa7eb9731c9468a592f1
	[aarch64]=cf29164de6379e17f6175d42b6a828a8e045c56575cf84db8183337cc6c682cb
)
declare -A LOADER=(
	[x86_64]=ld-linux-x86-64.so.2
	[aarch64]=ld-linux-aarch64.so.1
)
PYTHON_VERSION=3.14.7
PYTHON_ARCHIVE=python-${PYTHON_VERSION}-wasi_sdk-24.zip
PYTHON_SHA256=2e064d3fb8172471d39d741348efa722349c40b96301f69968dff714999c584b
HOST_VERSION=1
GLIBC_FLOOR=2.28
IMAGE=freshgram-plugin-host-builder
QEMU_TESTS="test_cbor test_vfs test_formats test_zip test_manifest test_package test_runtime test_broker test_runtime_bundle test_cache"

usage() {
	echo "usage: $0 <revision> [--arch x86_64|aarch64] [--min-revision N] [--out DIR] [--skip-tests]" >&2
	exit 2
}

[[ $# -ge 1 && $1 =~ ^[0-9]+$ ]] || usage
revision=$1
shift
min_revision=1
out=$HOME/freshgram-runtime
tests=1
arches=(x86_64 aarch64)
while [[ $# -gt 0 ]]; do
	case $1 in
		--arch) [[ ${2:-} == x86_64 || ${2:-} == aarch64 ]] || usage; arches=("$2"); shift 2 ;;
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

wasmtime_name() {
	echo "wasmtime-v$WASMTIME_VERSION-$1-linux-c-api"
}

build_dir() {
	[[ $1 == x86_64 ]] && echo "$host_dir/build-bundle" || echo "$host_dir/build-bundle-$1"
}

mkdir -p "$third_party/cpython-wasi"
for arch in x86_64 "${arches[@]}"; do
	name=$(wasmtime_name "$arch")
	fetch "https://github.com/bytecodealliance/wasmtime/releases/download/v$WASMTIME_VERSION/$name.tar.xz" \
		"$third_party/$name.tar.xz" "${WASMTIME_SHA256[$arch]}"
	if [[ ! -d $third_party/$name ]]; then
		tar -xJf "$third_party/$name.tar.xz" -C "$third_party"
	fi
done
fetch "https://github.com/brettcannon/cpython-wasi-build/releases/download/v$PYTHON_VERSION/$PYTHON_ARCHIVE" \
	"$third_party/cpython-wasi/$PYTHON_ARCHIVE" "$PYTHON_SHA256"
if [[ ! -f $third_party/cpython-wasi/python.wasm ]]; then
	unzip -q -o "$third_party/cpython-wasi/$PYTHON_ARCHIVE" -d "$third_party/cpython-wasi"
fi

docker build -q -t "$IMAGE" "$here" >/dev/null
in_docker() {
	local build=$1
	shift
	docker run --rm --security-opt seccomp=unconfined -u "$(id -u):$(id -g)" \
		-v "$telegram:/t:ro" -v "$build:/out" "$@"
}

build_tree() {
	local arch=$1 targets=$2 build
	build=$(build_dir "$arch")
	mkdir -p "$build"
	local configure="-DCMAKE_BUILD_TYPE=Release -DFGPL_WASMTIME_DIR=/t/plugins_host/third_party/$(wasmtime_name "$arch")"
	local strip="strip"
	if [[ $arch == aarch64 ]]; then
		configure="$configure -DCMAKE_TOOLCHAIN_FILE=/t/plugins_host/runtime/aarch64.cmake"
		strip="llvm-strip"
	fi
	echo "building the $arch host, log: $build/build.log"
	if ! in_docker "$build" "$IMAGE" bash -ec "
			source /opt/rh/gcc-toolset-13/enable
			cmake -S /t/plugins_host -B /out $configure
			cmake --build /out -j\$(nproc) $targets
			if [[ -f /out/fgpl_plugin_host ]]; then
				$strip -o /out/fgpl_plugin_host.stripped /out/fgpl_plugin_host
			fi" >"$build/build.log" 2>&1; then
		tail -30 "$build/build.log" >&2
		exit 1
	fi
}

test_tree() {
	local arch=$1 build
	build=$(build_dir "$arch")
	if [[ $arch == x86_64 ]]; then
		in_docker "$build" "$IMAGE" ctest --test-dir /out --output-on-failure -j4 | tail -3
		return
	elif ! command -v qemu-aarch64 >/dev/null; then
		echo "qemu-aarch64 is not installed, the aarch64 tests are skipped" >&2
		return
	fi
	if [[ ! -e $build/sysroot/lib ]]; then
		in_docker "$build" "$IMAGE" bash -ec "
			mkdir -p /out/sysroot
			cp -a /sysroot/aarch64/usr /out/sysroot/
			ln -sfn usr/lib64 /out/sysroot/lib64
			ln -sfn usr/lib /out/sysroot/lib"
	fi
	local failed=0
	for test in $QEMU_TESTS; do
		if ! (cd "$build" && qemu-aarch64 -L "$build/sysroot" "./$test" >"$build/$test.log" 2>&1); then
			echo "aarch64 $test failed, see $build/$test.log" >&2
			failed=1
		fi
	done
	[[ $failed == 0 ]] || exit 1
	echo "aarch64: $(echo $QEMU_TESTS | wc -w) suites passed under qemu-aarch64; the sandbox suites need an aarch64 kernel"
}

check_host() {
	local arch=$1 host=$2 needed glibc
	needed=$(readelf -d "$host" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
	for library in $needed; do
		case $library in
			libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|"${LOADER[$arch]}") ;;
			*) echo "the $arch host needs $library, which is not part of glibc" >&2; exit 1 ;;
		esac
	done
	glibc=$(readelf -V "$host" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
	if [[ $(printf '%s\n%s\n' "$glibc" "$GLIBC_FLOOR" | sort -V | tail -1) != "$GLIBC_FLOOR" ]]; then
		echo "the $arch host needs glibc $glibc, newer than $GLIBC_FLOOR" >&2
		exit 1
	fi
	echo "$arch host: glibc $glibc+, libraries: $(echo $needed)"
}

if [[ " ${arches[*]} " == *" x86_64 "* ]]; then
	build_tree x86_64 ""
else
	build_tree x86_64 "--target fgpl_tool"
fi
for arch in "${arches[@]}"; do
	[[ $arch == x86_64 ]] || build_tree "$arch" ""
	[[ $tests == 1 ]] && test_tree "$arch"
	check_host "$arch" "$(build_dir "$arch")/fgpl_plugin_host.stripped"
done

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/common/python" "$stage/common/sdk"
cp "$third_party/cpython-wasi/python.wasm" "$stage/common/python/"
cp -r "$third_party/cpython-wasi/lib" "$stage/common/python/lib"
cp -r "$telegram/plugins_sdk/python/freshgram" "$stage/common/sdk/freshgram"
find "$stage" -name __pycache__ -prune -exec rm -rf {} +
find "$stage" -name '*.pyc' -delete

hash_of() {
	echo "sha256-$(sha256sum "$1" | cut -d' ' -f1 | xxd -r -p | base64)"
}

mkdir -p "$target"
bundles=""
posts=""
for arch in "${arches[@]}"; do
	platform=linux-$arch
	placeholder=POST_ID_$(echo "$arch" | tr '[:lower:]' '[:upper:]')
	cp -r "$stage/common" "$stage/$arch"
	install -m 755 "$(build_dir "$arch")/fgpl_plugin_host.stripped" "$stage/$arch/fgpl_plugin_host"
	chmod -R a+rX "$stage"
	in_docker "$(build_dir x86_64)" -v "$stage:/stage:ro" -v "$target:/target" "$IMAGE" \
		/out/fgpl_tool pack-runtime "/stage/$arch" "/target/runtime-$platform.zip" >/dev/null
	bundle=$target/runtime-$platform.zip
	entry="\"$platform\":{\"post\":$placeholder,\"sha256\":\"$(hash_of "$bundle")\",\"size\":$(stat -c %s "$bundle"),\"hostSha256\":\"$(hash_of "$stage/$arch/fgpl_plugin_host")\",\"pythonSha256\":\"$(hash_of "$stage/$arch/python/python.wasm")\"}"
	bundles=${bundles:+$bundles,}$entry
	posts="$posts
     $(basename "$bundle") -> $placeholder"
done

cat > "$target/runtime.unsigned.json" <<EOF
{"schema":1,"revision":$revision,"minRevision":$min_revision,"host":$HOST_VERSION,"wasmtime":"$WASMTIME_VERSION","python":"$PYTHON_VERSION","revoked":[],"bundles":{$bundles}}
EOF

cat <<EOF

Bundles and index: $target

Next:
  1. Post every bundle to @freshgram_runtime as a file and put its post id in place of its
     placeholder in $target/runtime.unsigned.json:$posts
  2. Sign the index, for example with the tool built here:
     docker run --rm -u \$(id -u):\$(id -g) -v $(build_dir x86_64):/out:ro -v $target:/target \\
       -v <runtime.key>:/runtime.key:ro $IMAGE \\
       /out/fgpl_tool sign-runtime /target/runtime.unsigned.json /runtime.key /target/runtime.json
  3. Post runtime.json to the channel as a file and pin that post.
EOF
