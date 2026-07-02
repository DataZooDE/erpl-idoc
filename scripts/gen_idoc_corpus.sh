#!/usr/bin/env bash
# Mass-produce a corpus of standard SAP IDoc types for performance and compatibility
# testing of erpl_idoc — in your choice of FORMAT: flat (binary), xml, or both.
#
# HOW: deploy + run an ABAP generator (test/e2e/abap/zcl_erpl_idoc_gen.abap) via
# erpl-adt. For each basic type the generator reads the real segment structure from
# the SAP DDIC and writes (a) a byte-exact flat IDoc (EDI_DC40 + one EDI_DD40 per
# segment) and (b) a per-type dictionary CSV. This script copies them out, and:
#   - FORMAT=flat : keeps the .idoc files
#   - FORMAT=xml  : converts each flat IDoc -> IDoc-XML with erpl_idoc (sap_idoc_to_xml)
#   - FORMAT=both : keeps both
# then verifies every produced file parses with erpl_idoc, and (optionally) builds
# large multi-IDoc files for volume benchmarks.
#
# BOUNDARY: erpl_idoc never does RFC. Generation happens in ABAP *on SAP* (reached
# via erpl-adt/ADT, not RFC); erpl_idoc is used only to read/convert the files.
#
# Usage:
#   scripts/gen_idoc_corpus.sh [OUTPUT_DIR]
# Env:
#   FORMAT=flat|xml|both                 # output format (default: flat)
#   TYPES="MATMAS05 DEBMAS07 ORDERS05"   # override the built-in default type list
#   TYPES_FILE=path/to/list.txt          # ...or a file ("IDOCTYP [VERSION]" per line)
#   REPEAT=1000                          # also emit volume/ files (see below)
#   MIXED=1                              # also emit volume/all_mixed.idoc (flat only)
#   (SAP connection + extension paths come from test/e2e/e2e_common.sh defaults)
#
# Volume (REPEAT>1): flat -> volume/<TYPE>.xN.idoc (one N-IDoc file, ideal for
# scan-throughput benchmarks); xml -> volume/<TYPE>.<i>.xml copies (glob them, since
# concatenating single-IDoc XML docs is not one valid document).
set -uo pipefail

# Reuse connection defaults, DUCKDB, and the erpl_idoc extension path. Sourcing does
# NOT require erpl_rfc (we don't call e2e_preflight) — generation is via erpl-adt and
# verification via erpl_idoc alone.
source "$(dirname "${BASH_SOURCE[0]}")/../test/e2e/e2e_common.sh"

OUT_DIR="${1:-corpus}"
FORMAT="${FORMAT:-flat}"          # flat | xml | both
REPEAT="${REPEAT:-1}"
MIXED="${MIXED:-0}"
CLASS="ZCL_ERPL_IDOC_GEN"
ABAP_FILE="test/e2e/abap/zcl_erpl_idoc_gen.abap"
CONTAINER_DIR="/tmp/idoc_corpus"

die(){ echo "ERROR: $1" >&2; exit 1; }
case "$FORMAT" in flat|xml|both) ;; *) die "FORMAT must be flat|xml|both (got '$FORMAT')";; esac
want_flat(){ [ "$FORMAT" = flat ] || [ "$FORMAT" = both ]; }
want_xml(){  [ "$FORMAT" = xml  ] || [ "$FORMAT" = both ]; }

command -v uvx    >/dev/null || die "uvx (erpl-adt) not available"
command -v docker >/dev/null || die "docker not available"
docker ps --format '{{.Names}}' | grep -qx a4h || die "a4h container not running"
[ -f "$ERPL_IDOC_EXTENSION" ] || die "erpl_idoc extension not built ($ERPL_IDOC_EXTENSION) — run 'make debug'"
[ -x "$DUCKDB" ] || die "duckdb shell not built ($DUCKDB) — run 'make debug'"

mkdir -p "$OUT_DIR"
echo "== erpl_idoc corpus generator =="
echo "  output dir : $OUT_DIR"
echo "  format     : $FORMAT     repeat: $REPEAT   mixed: $MIXED"

# 1) Prepare the container corpus dir and (optionally) the type list.
docker exec a4h sh -c "mkdir -p $CONTAINER_DIR && chmod 0777 $CONTAINER_DIR"
if [ -n "${TYPES_FILE:-}" ]; then
  [ -f "$TYPES_FILE" ] || die "TYPES_FILE not found: $TYPES_FILE"
  docker exec -i a4h sh -c "cat > $CONTAINER_DIR/types.txt" < "$TYPES_FILE"
  echo "  types      : from $TYPES_FILE"
elif [ -n "${TYPES:-}" ]; then
  printf '%s\n' $TYPES | docker exec -i a4h sh -c "cat > $CONTAINER_DIR/types.txt"
  echo "  types      : $TYPES"
else
  docker exec a4h sh -c "rm -f $CONTAINER_DIR/types.txt"
  echo "  types      : built-in default list"
fi

# 2) Deploy + activate + run the ABAP generator via erpl-adt.
export SAP_PASSWORD="$ERPL_SAP_PASSWORD"
ADT(){ timeout 240 uvx erpl-adt --host "$ERPL_SAP_ASHOST" --port 50000 --user "$ERPL_SAP_USER" \
        --client "$ERPL_SAP_CLIENT" --password-env SAP_PASSWORD "$@"; }

echo "== deploying $CLASS =="
ADT object create --type CLAS/OC --name "$CLASS" --package '$TMP' \
    --description 'erpl_idoc corpus generator' >/dev/null 2>&1
ADT source write "$CLASS" --type CLAS --file "$ABAP_FILE" --activate >/dev/null 2>&1 \
    || die "erpl-adt source write/activate failed for $CLASS"

echo "== running $CLASS on A4H =="
RUN=$(ADT object run "$CLASS" 2>/dev/null)
echo "$RUN" | sed 's/^/  | /'
echo "$RUN" | grep -q '^DONE ' || die "generator did not complete (no DONE marker)"

# --- erpl_idoc helpers (verification / conversion; no RFC) --------------------
idoc_count(){    # $1 flat file -> #data records
  "$DUCKDB" -unsigned 2>/dev/null <<SQL | tr -d ' \r'
.mode csv
.header off
LOAD '$ERPL_IDOC_EXTENSION';
SELECT count(*) FROM sap_idoc_read('$1');
SQL
}
flat_to_xml(){   # $1 flat, $2 dict, $3 out.xml
  "$DUCKDB" -unsigned 2>/dev/null <<SQL
LOAD '$ERPL_IDOC_EXTENSION';
COPY (SELECT xml FROM sap_idoc_to_xml('$1','$2')) TO '$3' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '');
SQL
}
xml_fieldcount(){ # $1 xml file -> #rows (fields) read back
  "$DUCKDB" -unsigned 2>/dev/null <<SQL | tr -d ' \r'
.mode csv
.header off
LOAD '$ERPL_IDOC_EXTENSION';
SELECT count(*) FROM sap_idoc_read_xml('$1');
SQL
}

# 3) Copy each generated file out; build the requested format(s); verify.
SUMMARY="$OUT_DIR/manifest.csv"
echo "idoctyp,version,segments,flat_bytes,flat_records,xml_bytes,xml_fields,status" > "$SUMMARY"
ok=0 ; fail=0

while IFS= read -r line; do
  typ=$(sed -n 's/.* type=\([^ ]*\).*/\1/p'   <<<"$line")
  ver=$(sed -n 's/.* ver=\([^ ]*\).*/\1/p'    <<<"$line")
  segs=$(sed -n 's/.* segs=\([^ ]*\).*/\1/p'  <<<"$line")
  bytes=$(sed -n 's/.* bytes=\([^ ]*\).*/\1/p' <<<"$line")
  cpath=$(sed -n 's/.* path=\([^ ]*\).*/\1/p' <<<"$line")
  dpath=$(sed -n 's/.* dict=\([^ ]*\).*/\1/p' <<<"$line")

  flat_local="$OUT_DIR/$(basename "$cpath")"
  dict_local="$OUT_DIR/$(basename "$dpath")"
  xml_local="$OUT_DIR/${typ}.xml"

  docker exec a4h cat "$cpath" > "$flat_local" 2>/dev/null
  docker exec a4h cat "$dpath" > "$dict_local" 2>/dev/null

  status="OK"; recs=$(idoc_count "$flat_local"); recs=${recs:-0}
  abytes=$(wc -c < "$flat_local" | tr -d ' ')
  [ "$abytes" = "$bytes" ] && [ "$recs" = "$segs" ] || status="FLAT_MISMATCH(bytes=$abytes recs=$recs)"

  xbytes=""; xfields=""
  if want_xml && [ "$status" = OK ]; then
    if flat_to_xml "$flat_local" "$dict_local" "$xml_local"; then
      xbytes=$(wc -c < "$xml_local" | tr -d ' ')
      xfields=$(xml_fieldcount "$xml_local"); xfields=${xfields:-0}
      [ "${xbytes:-0}" -gt 0 ] && [ "${xfields:-0}" -gt 0 ] || status="XML_EMPTY"
    else
      status="XML_CONVERT_FAILED"
    fi
  fi

  # Drop the intermediate flat file when the user asked for xml only.
  want_flat || rm -f "$flat_local"

  [ "$status" = OK ] && ok=$((ok+1)) || fail=$((fail+1))
  echo "$typ,$ver,$segs,$bytes,$recs,${xbytes:-},${xfields:-},$status" >> "$SUMMARY"
  printf '  %-32s segs=%-4s flat=%-9s recs=%-4s%s %s\n' "$typ" "$segs" "$bytes" "$recs" \
         "$(want_xml && printf ' xml=%-8s fields=%-4s' "${xbytes:-0}" "${xfields:-0}")" "$status"
done < <(echo "$RUN" | grep '^GEN ')

echo "$RUN" | grep '^SKIP ' | while IFS= read -r s; do
  st=$(sed -n 's/.* type=\([^ ]*\).*/\1/p' <<<"$s")
  rs=$(sed -n 's/.* reason=\(.*\)$/\1/p'   <<<"$s")
  echo "$st,,,,,,,SKIP:$rs" >> "$SUMMARY"
  printf '  %-32s SKIP (%s)\n' "$st" "$rs"
done

# 4) Optional volume files.
if [ "$REPEAT" -gt 1 ] 2>/dev/null; then
  vdir="$OUT_DIR/volume"; mkdir -p "$vdir"
  echo "== building volume files (x$REPEAT) in $vdir =="
  if want_flat; then
    for f in "$OUT_DIR"/*.idoc; do
      [ -e "$f" ] || continue
      b=$(basename "$f" .idoc); out="$vdir/${b}.x${REPEAT}.idoc"; : > "$out"
      for _ in $(seq 1 "$REPEAT"); do cat "$f" >> "$out"; done
      printf '  %-40s %s bytes (flat, %s IDocs)\n' "$(basename "$out")" "$(wc -c < "$out" | tr -d ' ')" "$REPEAT"
    done
  fi
  if want_xml; then
    for f in "$OUT_DIR"/*.xml; do
      [ -e "$f" ] || continue
      b=$(basename "$f" .xml)
      for i in $(seq 1 "$REPEAT"); do cp "$f" "$vdir/${b}.${i}.xml"; done
      printf '  %-40s %s copies (xml)\n' "${b}.<1..${REPEAT}>.xml" "$REPEAT"
    done
  fi
fi
if [ "${MIXED:-0}" = "1" ] && want_flat; then
  mixed="$OUT_DIR/volume/all_mixed.idoc"; mkdir -p "$OUT_DIR/volume"; : > "$mixed"
  reps=$([ "$REPEAT" -gt 1 ] 2>/dev/null && echo "$REPEAT" || echo 1)
  echo "== building mixed-type flat volume file (each type x$reps) =="
  for _ in $(seq 1 "$reps"); do
    for f in "$OUT_DIR"/*.idoc; do [ -e "$f" ] && cat "$f" >> "$mixed"; done
  done
  printf '  %-40s %s bytes\n' "$(basename "$mixed")" "$(wc -c < "$mixed" | tr -d ' ')"
fi

echo "== done: $ok ok, $fail problem(s) — format=$FORMAT, manifest: $SUMMARY =="
[ "$fail" -eq 0 ]
