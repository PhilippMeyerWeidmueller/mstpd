#!/usr/bin/env bash
# Run clang-tidy over the mstpd and mstpctl sources listed in Makefile.am,
# focusing on undefined behavior and pointer misuse.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=makefile_sources.sh
. "$SCRIPT_DIR/makefile_sources.sh"
cd "$SCRIPT_DIR/../.."

CLANG_TIDY=${CLANG_TIDY:-clang-tidy}

if ! command -v "$CLANG_TIDY" >/dev/null; then
	echo "error: $CLANG_TIDY not found (apt install clang-tidy, or set CLANG_TIDY)" >&2
	exit 2
fi

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# Stand-in for the autoconf-generated config.h.
cat > "$TMP_DIR/config.h" <<EOF
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_CLOCK_GETTIME 1
#define PACKAGE_VERSION "lint"
EOF

# Undefined behavior, pointer/memory misuse, and common C mistakes.
CHECKS=(
	-*
	clang-analyzer-core.*
	clang-analyzer-security.*
	-clang-analyzer-security.insecureAPI.*
	clang-analyzer-unix.*
	clang-analyzer-optin.portability.UnixAPI
	clang-analyzer-deadcode.DeadStores
	bugprone-undefined-memory-manipulation
	bugprone-sizeof-expression
	bugprone-sizeof-container
	bugprone-suspicious-memset-usage
	bugprone-suspicious-memory-comparison
	bugprone-suspicious-string-compare
	bugprone-suspicious-realloc-usage
	bugprone-not-null-terminated-result
	# bugprone-multi-level-implicit-pointer-conversion
	bugprone-implicit-widening-of-multiplication-result
	# bugprone-narrowing-conversions
	bugprone-signed-char-misuse
	bugprone-integer-division
	bugprone-misplaced-pointer-arithmetic-in-alloc
	bugprone-posix-return
	bugprone-unused-return-value
	# bugprone-assignment-in-if-condition
	# bugprone-macro-parentheses
	bugprone-macro-repeated-side-effects
	bugprone-incorrect-roundings
	bugprone-bad-signal-to-kill-thread
	bugprone-signal-handler
	bugprone-spuriously-wake-up-functions
	bugprone-terminating-continue
	bugprone-too-small-loop-variable
	bugprone-branch-clone
	bugprone-redundant-branch-condition
	bugprone-infinite-loop
	# bugprone-reserved-identifier
	# bugprone-switch-missing-default-case
)
CHECKS_CSV=$(IFS=,; echo "${CHECKS[*]}")

CFLAGS=(
	-std=gnu11
	-D_REENTRANT -D__LINUX__ -D_GNU_SOURCE
	-DMSTPD_PID_FILE='"/var/run/mstpd.pid"'
	-I. -I"$TMP_DIR"
)
read -r -a CJSON_CFLAGS <<<"$(pkg-config --cflags libcjson 2>/dev/null || true)"
CFLAGS+=("${CJSON_CFLAGS[@]}")

mapfile -t FILES < <({ am_sources mstpd_SOURCES; am_sources mstpctl_SOURCES; } | sort -u)

echo "==> clang-tidy on ${#FILES[@]} files"
# Header filter limits diagnostics to this repo's own headers.
"$CLANG_TIDY" \
	--checks="$CHECKS_CSV" \
	--header-filter="^$PWD/.*" \
	"${FILES[@]}" -- "${CFLAGS[@]}"

echo "==> Done"
