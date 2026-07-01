#!/usr/bin/env bash
# Shared setup for erpl_idoc end-to-end tests against the LIVE A4H trial.
#
# These tests are NOT part of the portable sqllogictest suite: they load the
# sibling erpl_rfc extension (which links the NW RFC SDK) alongside erpl_idoc and
# talk to a real SAP system. erpl_idoc itself never calls RFC — all SAP access is
# via erpl_rfc at the SQL layer (SPEC FR-C2). Run them with `make e2e` when an A4H
# system is reachable; they self-skip if erpl_rfc / A4H are unavailable.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# --- connection defaults (override via env) ---------------------------------
export ERPL_SAP_ASHOST="${ERPL_SAP_ASHOST:-localhost}"
export ERPL_SAP_SYSNR="${ERPL_SAP_SYSNR:-00}"
export ERPL_SAP_CLIENT="${ERPL_SAP_CLIENT:-001}"
export ERPL_SAP_USER="${ERPL_SAP_USER:-DEVELOPER}"
export ERPL_SAP_PASSWORD="${ERPL_SAP_PASSWORD:-ABAPtr2023#00}"
export ERPL_SAP_LANG="${ERPL_SAP_LANG:-EN}"

# --- locate the extensions ---------------------------------------------------
ERPL_RFC_EXTENSION="${ERPL_RFC_EXTENSION:-/home/jr/Projects/datazoo/erpl/build/debug/repository/v1.5.4/linux_amd64/erpl_rfc.duckdb_extension}"
ERPL_IDOC_EXTENSION="${ERPL_IDOC_EXTENSION:-$REPO_ROOT/build/debug/extension/erpl_idoc/erpl_idoc.duckdb_extension}"
NWRFC_LIB="${NWRFC_LIB:-/home/jr/Projects/datazoo/erpl/nwrfcsdk/linux/lib}"
DUCKDB="${DUCKDB:-$REPO_ROOT/build/debug/duckdb}"

export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$NWRFC_LIB"
# Silence the benign NW RFC SDK init leak reported by our ASan debug build.
export ASAN_OPTIONS="${ASAN_OPTIONS:-}:detect_leaks=0:detect_odr_violation=0"

e2e_skip() { echo "SKIP: $1"; exit 0; }

e2e_preflight() {
	[ -x "$DUCKDB" ] || e2e_skip "duckdb debug shell not built ($DUCKDB) — run 'make debug'"
	[ -f "$ERPL_RFC_EXTENSION" ] || e2e_skip "erpl_rfc extension not found ($ERPL_RFC_EXTENSION)"
	[ -f "$ERPL_IDOC_EXTENSION" ] || e2e_skip "erpl_idoc extension not built ($ERPL_IDOC_EXTENSION)"
}

# SQL preamble: load both extensions and open the SAP secret.
e2e_preamble() {
	cat <<SQL
LOAD '$ERPL_RFC_EXTENSION';
LOAD '$ERPL_IDOC_EXTENSION';
CREATE SECRET a4h (TYPE sap_rfc, ASHOST '$ERPL_SAP_ASHOST', SYSNR '$ERPL_SAP_SYSNR',
                   CLIENT '$ERPL_SAP_CLIENT', USER '$ERPL_SAP_USER',
                   PASSWD '$ERPL_SAP_PASSWORD', LANG '$ERPL_SAP_LANG');
SQL
}

# Run SQL (preamble + given body) through the -unsigned shell; stderr dropped.
# The preamble's LOAD/CREATE SECRET result boxes are muted via .output so callers
# capture only their own query output.
e2e_run_sql() {
	{ echo ".output /dev/null"; e2e_preamble; echo ".output"; cat; } | "$DUCKDB" -unsigned 2>/dev/null
}

# Assertion helper: name, expected, actual
e2e_assert_eq() {
	if [ "$2" == "$3" ]; then
		echo "  ok: $1"
	else
		echo "  FAIL: $1"
		echo "     expected: [$2]"
		echo "     actual:   [$3]"
		E2E_FAILED=1
	fi
}

E2E_FAILED=0
