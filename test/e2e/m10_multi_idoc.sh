#!/usr/bin/env bash
# E2E (DoD, real system): a flat file with TWO IDocs survives flat -> IDoc-XML -> flat
# byte-exact (one XML root, two <IDOC>s), and the SECOND IDoc of the round-tripped file
# is ACCEPTED by A4H inbound. Reuses the M4 ABAP importer (IDOC_INBOUND_WRITE_TO_DB).
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight
command -v docker >/dev/null || e2e_skip "docker not available"
command -v uvx   >/dev/null || e2e_skip "uvx (erpl-adt) not available"
docker ps --format '{{.Names}}' | grep -qx a4h || e2e_skip "a4h container not running"

echo "== M10: two IDocs flat -> XML -> flat -> A4H inbound acceptance =="

PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

HOST_FILE=/tmp/erpl_idoc_e2e.idoc

# 1) Two IDocs -> one XML -> flat again, using ONLY erpl_idoc (no RFC).
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
rm -f "$HOST_FILE"
OUT=$("$DUCKDB" -unsigned -csv -noheader 2>/dev/null <<SQL
LOAD '$ERPL_IDOC_EXTENSION';
COPY (SELECT raw_record FROM (
        SELECT 0 AS g, record_index, raw_record FROM sap_idoc_read_raw('test/fixtures/flight.idoc')
        UNION ALL
        -- the second IDoc is distinguishable in SAP: airline LH -> AB
        SELECT 1, record_index, replace(raw_record::VARCHAR, 'LH 0400', 'AB 0400')::BLOB FROM sap_idoc_read_raw('test/fixtures/flight.idoc'))
      ORDER BY g, record_index) TO '$WORK/two.idoc' (FORMAT sap_idoc);
COPY (SELECT xml FROM sap_idoc_to_xml('$WORK/two.idoc','test/fixtures/flight_dict.csv'))
  TO '$WORK/two.xml' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '');
COPY (SELECT raw_record FROM sap_idoc_xml_to_records('$WORK/two.xml','test/fixtures/flight_dict.csv') ORDER BY record_index)
  TO '$WORK/back.idoc' (FORMAT sap_idoc);
COPY (SELECT raw_record FROM sap_idoc_xml_to_records('$WORK/two.xml','test/fixtures/flight_dict.csv')
      WHERE document_key = 2 ORDER BY record_index) TO '$HOST_FILE' (FORMAT sap_idoc);
SELECT count(*) FROM sap_idoc_read_control('$WORK/back.idoc');
SQL
)
e2e_assert_eq "two IDocs survive flat -> XML -> flat"            "2"    "$(echo "$OUT" | tail -1)"
cmp -s "$WORK/two.idoc" "$WORK/back.idoc" && echo "  ok: round trip is byte-exact" || { echo "FAIL: round trip differs"; exit 1; }
[ "$(grep -c '<IDOC ' "$WORK/two.xml")" -eq 2 ] && [ "$(grep -c '^<FLIGHTBOOKING' "$WORK/two.xml")" -eq 1 ] \
  && echo "  ok: one XML root holding two <IDOC>s" || { echo "FAIL: XML shape"; exit 1; }
[ "$(wc -c < "$HOST_FILE")" -eq 2650 ] || { echo "FAIL: second IDoc is not 2650 bytes"; exit 1; }

# 2) Push into the container and run the (already-deployed) importer.
docker exec -i a4h sh -c 'cat > /tmp/erpl_idoc_e2e.idoc' < "$HOST_FILE"
export SAP_PASSWORD="$ERPL_SAP_PASSWORD"
ADT(){ timeout 120 uvx erpl-adt --host "$ERPL_SAP_ASHOST" --port 50000 --user "$ERPL_SAP_USER" \
        --client "$ERPL_SAP_CLIENT" --password-env SAP_PASSWORD "$@"; }
ADT object create --type CLAS/OC --name ZCL_ERPL_IDOC_E2E --package '$TMP' --description 'erpl_idoc E2E inbound' >/dev/null 2>&1
ADT source write ZCL_ERPL_IDOC_E2E --type CLAS --file test/e2e/abap/zcl_erpl_idoc_e2e.abap --activate >/dev/null 2>&1
RUN=$(ADT object run ZCL_ERPL_IDOC_E2E 2>/dev/null)

DOCNUM=$(echo "$RUN" | sed -n 's/^DOCNUM=//p' | tr -d '\r')
SDATA=$(echo "$RUN"  | sed -n 's/^SDATA=//p'  | tr -d '\r')
SEGCOUNT=$(echo "$RUN" | sed -n 's/^SEGCOUNT=//p' | tr -d '\r')
echo "  A4H stored docnum=$DOCNUM segcount=$SEGCOUNT sdata='$SDATA'"

[ -n "$DOCNUM" ] && [ "$DOCNUM" != "0000000000000000" ] || { echo "FAIL: no docnum"; exit 1; }
e2e_assert_eq "SAP re-read of the segment from the second IDoc" "AB 04002026071500000042Y" "$SDATA"
e2e_assert_eq "SAP stored both segments"                            "2"                        "$SEGCOUNT"

[ "$E2E_FAILED" -eq 0 ] && echo "M10 multi-IDoc E2E: PASS (A4H accepted the second IDoc of the round-tripped file)" || { echo "M10 multi-IDoc E2E: FAIL"; exit 1; }
