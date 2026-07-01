#!/usr/bin/env bash
# E2E (DoD, real system): an IDoc-XML file, converted to flat by erpl_idoc, is
# ACCEPTED by A4H inbound and its segments are stored — proving the XML path is
# system-true. Reuses the M4 ABAP importer (IDOC_INBOUND_WRITE_TO_DB).
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight
command -v docker >/dev/null || e2e_skip "docker not available"
command -v uvx   >/dev/null || e2e_skip "uvx (erpl-adt) not available"
docker ps --format '{{.Names}}' | grep -qx a4h || e2e_skip "a4h container not running"

echo "== M7: IDoc-XML -> flat -> A4H inbound acceptance =="

PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

HOST_FILE=/tmp/erpl_idoc_e2e.idoc

# 1) Convert the IDoc-XML fixture to a flat IDoc using ONLY erpl_idoc (no RFC).
"$DUCKDB" -unsigned 2>/dev/null <<SQL
LOAD '$ERPL_IDOC_EXTENSION';
COPY (SELECT raw_record FROM sap_idoc_xml_to_records('test/fixtures/flight.xml','test/fixtures/flight_dict.csv')
      ORDER BY record_index)
  TO '$HOST_FILE' (FORMAT sap_idoc);
SQL
[ "$(wc -c < "$HOST_FILE")" -eq 2650 ] || { echo "FAIL: xml->flat did not produce a 2650-byte file"; exit 1; }
echo "  ok: erpl_idoc converted flight.xml -> $(wc -c < "$HOST_FILE")-byte flat IDoc"

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
e2e_assert_eq "SAP re-read of the segment from the XML-derived IDoc" "LH 04002026071500000042Y" "$SDATA"
e2e_assert_eq "SAP stored both segments"                            "2"                        "$SEGCOUNT"

[ "$E2E_FAILED" -eq 0 ] && echo "M7 XML E2E: PASS (A4H accepted the XML-derived IDoc)" || { echo "M7 XML E2E: FAIL"; exit 1; }
