#!/usr/bin/env bash
# E2E (DoD, real system): an IDoc stored in SAP (EDIDC + EDID4, read through erpl_rfc) is turned
# into a flat IDoc file by sql/idoc_from_edidc_edid4.sql, and the file agrees with SAP:
#   - control record and segment structure equal what EDIDC / EDID4 hold,
#   - A4H inbound ACCEPTS the file (reuses the M4 ABAP importer) and re-reads the same data.
# Needs an IDoc of type FLIGHTBOOKING_CREATEFROMDAT01 in EDIDC (run m4/m7/m10 first if none).
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight
command -v docker >/dev/null || e2e_skip "docker not available"
command -v uvx   >/dev/null || e2e_skip "uvx (erpl-adt) not available"
docker ps --format '{{.Names}}' | grep -qx a4h || e2e_skip "a4h container not running"

echo "== M11: EDIDC/EDID4 -> flat IDoc file -> A4H =="

PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

# one-value query against the live system (erpl_rfc preamble + erpl_idoc)
sql_value() { { echo ".output /dev/null"; e2e_preamble; echo ".output"; cat; } | "$DUCKDB" -unsigned -list -noheader 2>/dev/null | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' | tail -1; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
FILE="$WORK/from_tables.idoc"
HOST_FILE=/tmp/erpl_idoc_e2e.idoc
rm -f "$HOST_FILE"

# 1) pick a real IDoc that SAP holds
DOCNUM=$(sql_value <<'SQL'
SELECT min(DOCNUM) FROM sap_read_table('EDIDC', FILTER='IDOCTP EQ ''FLIGHTBOOKING_CREATEFROMDAT01''');
SQL
)
[ -n "$DOCNUM" ] && [ "$DOCNUM" != "NULL" ] || e2e_skip "no FLIGHTBOOKING_CREATEFROMDAT01 IDoc in EDIDC (run m4 first)"
echo "  source IDoc in SAP: DOCNUM=$DOCNUM"

# 2) run the recipe file itself (placeholders substituted), with erpl_rfc + erpl_idoc loaded
sed -e "s|<<<DOCNUM>>>|$DOCNUM|g" -e "s|<<<OUT>>>|$FILE|g" sql/idoc_from_edidc_edid4.sql > "$WORK/recipe.sql"
{ e2e_preamble; cat "$WORK/recipe.sql"; } | "$DUCKDB" -unsigned >"$WORK/recipe.out" 2>&1 \
  || { echo "FAIL: recipe errored"; tail -5 "$WORK/recipe.out"; exit 1; }
[ -s "$FILE" ] || { echo "FAIL: recipe wrote no file"; tail -5 "$WORK/recipe.out"; exit 1; }

# 3) SAP truth, read from the tables WITHOUT the recipe (metadata columns only; SDATA is LCHR)
SAP_STRUCT=$(sql_value <<SQL
SELECT string_agg(SEGNUM || ':' || trim(SEGNAM) || ':' || PSGNUM || ':' || HLEVEL, ',' ORDER BY SEGNUM)
FROM sap_read_table('EDID4', FILTER='DOCNUM EQ ''$DOCNUM''');
SQL
)
# all 36 EDI_DC40 values, in file order, '|'-joined (TABNAM is the constant EDI_DC40; CREDAT/CRETIM as digits)
SAP_CTRL=$(sql_value <<SQL
SELECT array_to_string(list_transform(['EDI_DC40', MANDT, DOCNUM, DOCREL, STATUS, DIRECT, OUTMOD, EXPRSS, TEST,
       IDOCTP, CIMTYP, MESTYP, MESCOD, MESFCT, STD, STDVRS, STDMES, SNDPOR, SNDPRT, SNDPFC, SNDPRN, SNDSAD, SNDLAD,
       RCVPOR, RCVPRT, RCVPFC, RCVPRN, RCVSAD, RCVLAD,
       left(regexp_replace(CAST(CREDAT AS VARCHAR), '[^0-9]', '', 'g'), 8),
       left(regexp_replace(CAST(CRETIM AS VARCHAR), '[^0-9]', '', 'g'), 6),
       REFINT, REFGRP, REFMES, ARCKEY, SERIAL], x -> COALESCE(x, '')), '|')
FROM sap_read_table('EDIDC', FILTER='DOCNUM EQ ''$DOCNUM''');
SQL
)
# the full SDATA of EVERY segment, hashed (SDATA only comes through /SAPDS/RFC_READ_TABLE2)
SAP_ALLSDATA=$(sql_value <<SQL
SELECT md5(string_agg(rtrim(substr(u.WA, 45)), '~' ORDER BY substr(u.WA, 1, 6)))
FROM sap_rfc_invoke('/SAPDS/RFC_READ_TABLE2',
       {'QUERY_TABLE':'EDID4',
        'FIELDS':[{'FIELDNAME':'SEGNUM'},{'FIELDNAME':'SEGNAM'},{'FIELDNAME':'PSGNUM'},{'FIELDNAME':'HLEVEL'},{'FIELDNAME':'SDATA'}],
        'OPTIONS':[{'TEXT':'DOCNUM EQ ''$DOCNUM'''}]}) r, UNNEST(r.TBLOUT2048) AS t(u);
SQL
)
SAP_SDATA=$(sql_value <<SQL
SELECT rtrim(substr(substr(u.WA, 45), 1, 40))
FROM sap_rfc_invoke('/SAPDS/RFC_READ_TABLE2',
       {'QUERY_TABLE':'EDID4',
        'FIELDS':[{'FIELDNAME':'SEGNUM'},{'FIELDNAME':'SEGNAM'},{'FIELDNAME':'PSGNUM'},{'FIELDNAME':'HLEVEL'},{'FIELDNAME':'SDATA'}],
        'OPTIONS':[{'TEXT':'DOCNUM EQ ''$DOCNUM'''}]}) r, UNNEST(r.TBLOUT2048) AS t(u)
WHERE trim(substr(u.WA, 7, 30)) = 'E1BPSBONEW';
SQL
)

# 4) the file, read back with erpl_idoc only
FILE_STRUCT=$("$DUCKDB" -unsigned -list -noheader 2>/dev/null <<SQL | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' | tail -1
LOAD '$ERPL_IDOC_EXTENSION';
SELECT string_agg(segnum || ':' || segnam || ':' || psgnum || ':' || hlevel, ',' ORDER BY segnum)
FROM sap_idoc_read('$FILE');
SQL
)
FILE_CTRL=$("$DUCKDB" -unsigned -list -noheader 2>/dev/null <<SQL | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' | tail -1
LOAD '$ERPL_IDOC_EXTENSION';
SELECT array_to_string(list_transform([tabnam, mandt, docnum, docrel, status, direct, outmod, exprss, test,
       idoctyp, cimtyp, mestyp, mescod, mesfct, std, stdvrs, stdmes, sndpor, sndprt, sndpfc, sndprn, sndsad, sndlad,
       rcvpor, rcvprt, rcvpfc, rcvprn, rcvsad, rcvlad, credat, cretim, refint, refgrp, refmes, arckey, serial],
       x -> COALESCE(x, '')), '|')
FROM sap_idoc_read_control('$FILE');
SQL
)
FILE_ALLSDATA=$("$DUCKDB" -unsigned -list -noheader 2>/dev/null <<SQL | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' | tail -1
LOAD '$ERPL_IDOC_EXTENSION';
SELECT md5(string_agg(rtrim(sdata), '~' ORDER BY segnum)) FROM sap_idoc_read('$FILE');
SQL
)
NSEG=$(echo "$SAP_STRUCT" | tr ',' '\n' | grep -c .)
[ -n "$SAP_STRUCT" ] && [ -n "$SAP_CTRL" ] && [ -n "$SAP_SDATA" ] && [ -n "$SAP_ALLSDATA" ] || { echo "FAIL: could not read the source IDoc back from SAP (struct='$SAP_STRUCT' ctrl='$SAP_CTRL' sdata='$SAP_SDATA')"; exit 1; }
echo "  SAP says: $SAP_CTRL  segments: $SAP_STRUCT"
echo "  SAP E1BPSBONEW SDATA: '$SAP_SDATA'"
e2e_assert_eq "file size = control + segments x 1063" "$((524 + 1063 * NSEG))" "$(wc -c < "$FILE")"
e2e_assert_eq "segment structure (SEGNUM:SEGNAM:PSGNUM:HLEVEL) equals EDID4" "$SAP_STRUCT" "$FILE_STRUCT"
e2e_assert_eq "all 36 control fields equal EDIDC (IDOCTP as IDOCTYP, CREDAT/CRETIM as digits)" "$SAP_CTRL" "$FILE_CTRL"
e2e_assert_eq "full SDATA of every segment equals EDID4 (md5)" "$SAP_ALLSDATA" "$FILE_ALLSDATA"

# 5) A4H accepts the file and re-reads the same payload from its own storage
docker exec -i a4h sh -c 'cat > /tmp/erpl_idoc_e2e.idoc' < "$FILE" || { echo "FAIL: could not push the file into a4h"; exit 1; }
export SAP_PASSWORD="$ERPL_SAP_PASSWORD"
ADT(){ timeout 120 uvx erpl-adt --host "$ERPL_SAP_ASHOST" --port 50000 --user "$ERPL_SAP_USER" \
        --client "$ERPL_SAP_CLIENT" --password-env SAP_PASSWORD "$@"; }
ADT object create --type CLAS/OC --name ZCL_ERPL_IDOC_E2E --package '$TMP' --description 'erpl_idoc E2E inbound' >/dev/null 2>&1
ADT source write ZCL_ERPL_IDOC_E2E --type CLAS --file test/e2e/abap/zcl_erpl_idoc_e2e.abap --activate >/dev/null 2>&1
RUN=$(ADT object run ZCL_ERPL_IDOC_E2E 2>/dev/null)
NEWDOC=$(echo "$RUN" | sed -n 's/^DOCNUM=//p' | tr -d '\r')
IMP_SDATA=$(echo "$RUN" | sed -n 's/^SDATA=//p' | tr -d '\r' | sed 's/ *$//')
IMP_SEGS=$(echo "$RUN" | sed -n 's/^SEGCOUNT=//p' | tr -d '\r')
echo "  A4H stored the file as docnum=$NEWDOC"
[ -n "$NEWDOC" ] && [ "$NEWDOC" != "0000000000000000" ] && [ "$NEWDOC" != "$DOCNUM" ] || { echo "FAIL: A4H did not store a new IDoc"; exit 1; }
e2e_assert_eq "A4H stored every segment"                        "$NSEG"      "$IMP_SEGS"
e2e_assert_eq "A4H's re-read E1BPSBONEW SDATA = the source IDoc's" "$SAP_SDATA" "$IMP_SDATA"

[ "$E2E_FAILED" -eq 0 ] && echo "M11 EDIDC/EDID4 E2E: PASS (file agrees with SAP's tables and A4H accepted it)" || { echo "M11 EDIDC/EDID4 E2E: FAIL"; exit 1; }
