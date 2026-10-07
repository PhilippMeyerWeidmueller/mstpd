#!/usr/bin/env bash
# Compile mstpd, mstpctl and unit tests with gcc using strict warnings.
# Usage: .devcontainer/scripts/build_strict.sh [-t]
#   -t  also build and run unit tests (requires cmocka)
# The build directory is wiped on every run.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=makefile_sources.sh
. "$SCRIPT_DIR/makefile_sources.sh"
cd "$SCRIPT_DIR/../.."

BUILD_DIR=build
CC=${CC:-gcc}
RUN_TESTS=0

while getopts "t" opt; do
	case $opt in
	t) RUN_TESTS=1 ;;
	*) sed -n '2,5p' "$0"; exit 1 ;;
	esac
done

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# Stand-in for the autoconf-generated config.h.
cat > "$BUILD_DIR/config.h" <<EOF
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_CLOCK_GETTIME 1
#define PACKAGE_VERSION "dev-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
EOF

CFLAGS=(
	-std=gnu11
	-g3
	-O2
	-Wno-unused-parameter
	-Wno-sign-compare
	-Wall
	-Wextra
	-Wformat=2
	-Wformat-security
	-fstack-protector-strong
	-D_FORTIFY_SOURCE=2
	-D_REENTRANT
	-D__LINUX__
	-D_GNU_SOURCE
	-DMSTPD_PID_FILE='"/var/run/mstpd.pid"'
	-I.
	-I"$BUILD_DIR"
)

read -r -a CJSON_CFLAGS <<<"$(pkg-config --cflags libcjson)"
read -r -a CJSON_LIBS <<<"$(pkg-config --libs libcjson)"
CFLAGS+=("${CJSON_CFLAGS[@]}")

mapfile -t MSTPD_SRCS < <(am_sources mstpd_SOURCES)
mapfile -t MSTPCTL_SRCS < <(am_sources mstpctl_SOURCES)

echo "==> Building mstpd"
"$CC" "${CFLAGS[@]}" "${MSTPD_SRCS[@]}" -o "$BUILD_DIR/mstpd" "${CJSON_LIBS[@]}" -lm -lrt

echo "==> Building mstpctl"
"$CC" "${CFLAGS[@]}" "${MSTPCTL_SRCS[@]}" -o "$BUILD_DIR/mstpctl" -lrt

if ((RUN_TESTS)); then
	read -r -a CMOCKA_CFLAGS <<<"$(pkg-config --cflags cmocka)"
	read -r -a CMOCKA_LIBS <<<"$(pkg-config --libs cmocka)"
	for prog in $(am_var check_PROGRAMS); do
		t=$(basename "$prog")
		mapfile -t TEST_SRCS < <(am_sources "$(am_canon "$prog")_SOURCES")
		echo "==> Building and running $t"
		"$CC" "${CFLAGS[@]}" "${CMOCKA_CFLAGS[@]}" "${TEST_SRCS[@]}" \
			-o "$BUILD_DIR/$t" "${CMOCKA_LIBS[@]}" -lrt
		"$BUILD_DIR/$t"
	done
fi

echo "==> Done: $BUILD_DIR/mstpd, $BUILD_DIR/mstpctl"
