#!/usr/bin/env bash
# export_idoc_dict.sh — create an offline IDoc segment-dictionary file from a live
# SAP system, by composing erpl_rfc in SQL (IDOCTYPE_READ_COMPLETE). This is the
# ergonomic wrapper around sql/sap_idoc_dictionary.sql: a DuckDB table MACRO cannot
# wrap sap_rfc_invoke (it evaluates its args at bind time), so this script substitutes
# the concrete basic type into the query and COPYs the result to CSV or Parquet.
#
# The dictionary matches the SPEC B4 schema and is thereafter usable fully offline
# (no SAP, no erpl_rfc) by read_idoc_segment(...).
#
# Usage:
#   scripts/export_idoc_dict.sh IDOCTYP [--cimtyp EXT] [--version 4]
#                               [--format csv|parquet] [--out PATH] [--all-segments]
#   scripts/export_idoc_dict.sh --batch types.txt [--format parquet] [--out-dir DIR]
#
# Connection + extension locations come from env (same names as the E2E harness):
#   ERPL_SAP_ASHOST/SYSNR/CLIENT/USER/PASSWORD/LANG, ERPL_RFC_EXTENSION,
#   ERPL_IDOC_EXTENSION, DUCKDB, NWRFC_LIB.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ERPL_SAP_ASHOST="${ERPL_SAP_ASHOST:-localhost}"
ERPL_SAP_SYSNR="${ERPL_SAP_SYSNR:-00}"
ERPL_SAP_CLIENT="${ERPL_SAP_CLIENT:-001}"
ERPL_SAP_USER="${ERPL_SAP_USER:-DEVELOPER}"
ERPL_SAP_PASSWORD="${ERPL_SAP_PASSWORD:-ABAPtr2023#00}"
ERPL_SAP_LANG="${ERPL_SAP_LANG:-EN}"
ERPL_RFC_EXTENSION="${ERPL_RFC_EXTENSION:-/home/jr/Projects/datazoo/erpl/build/debug/repository/v1.5.4/linux_amd64/erpl_rfc.duckdb_extension}"
ERPL_IDOC_EXTENSION="${ERPL_IDOC_EXTENSION:-$REPO_ROOT/build/debug/extension/erpl_idoc/erpl_idoc.duckdb_extension}"
NWRFC_LIB="${NWRFC_LIB:-/home/jr/Projects/datazoo/erpl/nwrfcsdk/linux/lib}"
DUCKDB="${DUCKDB:-$REPO_ROOT/build/debug/duckdb}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$NWRFC_LIB"
export ASAN_OPTIONS="${ASAN_OPTIONS:-}:detect_leaks=0:detect_odr_violation=0"

die(){ echo "error: $*" >&2; exit 1; }

CIMTYP=""; VERSION="4"; FORMAT="parquet"; OUT=""; OUTDIR="."; BATCH=""; ONE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --cimtyp)  CIMTYP="$2"; shift 2;;
    --version) VERSION="$2"; shift 2;;
    --format)  FORMAT="$2"; shift 2;;
    --out)     OUT="$2"; shift 2;;
    --out-dir) OUTDIR="$2"; shift 2;;
    --batch)   BATCH="$2"; shift 2;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    -*)        die "unknown flag $1";;
    *)         ONE="$1"; shift;;
  esac
done
[ "$FORMAT" = csv ] || [ "$FORMAT" = parquet ] || die "--format must be csv or parquet"
[ -x "$DUCKDB" ] || die "duckdb shell not found ($DUCKDB) — run 'make debug'"
[ -f "$ERPL_RFC_EXTENSION" ] || die "erpl_rfc extension not found ($ERPL_RFC_EXTENSION)"

# Emit the dictionary SELECT for one (idoctyp, cimtyp) — mirrors sql/sap_idoc_dictionary.sql.
dict_select() {
  local idoctyp="$1" cimtyp="$2"
  cat <<SQL
WITH raw AS (
  SELECT PE_HEADER, PT_FIELDS, PT_SEGMENTS FROM sap_rfc_invoke('IDOCTYPE_READ_COMPLETE',
    {'PI_IDOCTYP':'$idoctyp','PI_CIMTYP':'$cimtyp','PI_VERSION':'$VERSION'})
),
segs AS (SELECT s.SEGMENTTYP AS seg, s.SEGMENTDEF AS segdef FROM raw, UNNEST(PT_SEGMENTS) AS t(s))
SELECT '$idoctyp' AS idoctyp, '$cimtyp' AS cimtyp, (SELECT PE_HEADER.RELEASED FROM raw) AS release,
       f.SEGMENTTYP AS segnam, (SELECT segdef FROM segs WHERE seg=f.SEGMENTTYP LIMIT 1) AS segdef,
       CAST(f.FIELD_POS AS INTEGER) AS field_pos, f.FIELDNAME AS field_name,
       CAST(f.BYTE_FIRST AS INTEGER) AS "offset", CAST(f.EXTLEN AS INTEGER) AS length,
       f.DATATYPE AS datatype, f.ROLLNAME AS data_element, f.DESCRP AS description, false AS mandatory
FROM raw, UNNEST(PT_FIELDS) AS t(f) ORDER BY segnam, field_pos
SQL
}

export_one() {
  local idoctyp="$1" cimtyp="$2" out="$3"
  local copts="FORMAT $FORMAT"
  [ "$FORMAT" = csv ] && copts="FORMAT csv, HEADER true"   # HEADER is a CSV-only option
  { echo ".output /dev/null"
    echo "LOAD '$ERPL_RFC_EXTENSION'; LOAD '$ERPL_IDOC_EXTENSION';"
    echo "CREATE SECRET s (TYPE sap_rfc, ASHOST '$ERPL_SAP_ASHOST', SYSNR '$ERPL_SAP_SYSNR', CLIENT '$ERPL_SAP_CLIENT', USER '$ERPL_SAP_USER', PASSWD '$ERPL_SAP_PASSWORD', LANG '$ERPL_SAP_LANG');"
    echo ".output"
    echo "COPY ( $(dict_select "$idoctyp" "$cimtyp") ) TO '$out' ($copts);"
  } | "$DUCKDB" -unsigned 2>/dev/null
  [ -s "$out" ] || die "export produced no file for $idoctyp (SAP unreachable, or type unknown?)"
  echo "wrote $out"
}

ext() { [ "$FORMAT" = csv ] && echo csv || echo parquet; }

if [ -n "$BATCH" ]; then
  [ -f "$BATCH" ] || die "batch file not found: $BATCH"
  mkdir -p "$OUTDIR"
  while read -r idoctyp cimtyp _; do
    [ -z "${idoctyp// }" ] && continue
    case "$idoctyp" in \#*) continue;; esac
    export_one "$idoctyp" "${cimtyp:-}" "$OUTDIR/${idoctyp}.dict.$(ext)"
  done < "$BATCH"
else
  [ -n "$ONE" ] || die "give an IDOCTYP (or --batch FILE). See --help."
  export_one "$ONE" "$CIMTYP" "${OUT:-${ONE}.dict.$(ext)}"
fi
