#!/usr/bin/env bash
# E2E (DoD, real system): sap_idoc_read_segment(typed := true) agrees with SAP.
# The flat IDoc is accepted by A4H inbound (IDOC_INBOUND_WRITE_TO_DB via the M4 importer) and
# re-read from SAP's own storage; the typed DATE values erpl_idoc derives from the SAME bytes must
# format back to exactly the date text SAP stored. Reuses the M4 ABAP importer (see m7).
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight
command -v docker >/dev/null || e2e_skip "docker not available"
command -v uvx   >/dev/null || e2e_skip "uvx (erpl-adt) not available"
docker ps --format '{{.Names}}' | grep -qx a4h || e2e_skip "a4h container not running"

echo "== M8: typed columns vs SAP's stored IDoc =="

PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

HOST_FILE=/tmp/erpl_idoc_e2e.idoc
cp test/fixtures/flight.idoc "$HOST_FILE"

# 1) Let A4H ingest the file and read it back from its own storage (SAP truth).
docker exec -i a4h sh -c 'cat > /tmp/erpl_idoc_e2e.idoc' < "$HOST_FILE" || { echo "FAIL: could not push the IDoc into a4h"; exit 1; }
docker exec -i a4h sh -c 'cmp -s /tmp/erpl_idoc_e2e.idoc -' < "$HOST_FILE" || { echo "FAIL: a4h copy differs from the fixture"; exit 1; }
export SAP_PASSWORD="$ERPL_SAP_PASSWORD"
ADT(){ timeout 120 uvx erpl-adt --host "$ERPL_SAP_ASHOST" --port 50000 --user "$ERPL_SAP_USER" \
        --client "$ERPL_SAP_CLIENT" --password-env SAP_PASSWORD "$@"; }
ADT object create --type CLAS/OC --name ZCL_ERPL_IDOC_E2E --package '$TMP' --description 'erpl_idoc E2E inbound' >/dev/null 2>&1
ADT source write ZCL_ERPL_IDOC_E2E --type CLAS --file test/e2e/abap/zcl_erpl_idoc_e2e.abap --activate >/dev/null 2>&1
RUN=$(ADT object run ZCL_ERPL_IDOC_E2E 2>/dev/null)

DOCNUM=$(echo "$RUN" | sed -n 's/^DOCNUM=//p' | tr -d '\r')
SDATA=$(echo "$RUN"  | sed -n 's/^SDATA=//p'  | tr -d '\r')
echo "  A4H stored docnum=$DOCNUM sdata='$SDATA'"
[ -n "$DOCNUM" ] && [ "$DOCNUM" != "0000000000000000" ] || { echo "FAIL: no docnum"; exit 1; }

# SAP's text for FLIGHTDATE: dictionary offset 7, length 8 (1-based substr start 8)
SAP_FLIGHTDATE="${SDATA:7:8}"
e2e_assert_eq "SAP stored FLIGHTDATE text" "20260715" "$SAP_FLIGHTDATE"

# 2) erpl_idoc typed read of the same bytes (no RFC): DATE type, and it formats back to SAP's text.
TYPED=$("$DUCKDB" -unsigned -list -noheader 2>/dev/null <<SQL
LOAD '$ERPL_IDOC_EXTENSION';
SELECT typeof(flightdate) || ',' || strftime(flightdate, '%Y%m%d') || ',' || typeof(connectid)
FROM sap_idoc_read_segment('$HOST_FILE', 'E1BPSBONEW', 'test/fixtures/flight_dict.csv', typed := true);
SQL
)
e2e_assert_eq "typed read: DATE column, formats back to SAP's text, NUMC stays text" \
              "DATE,${SAP_FLIGHTDATE},VARCHAR" "$(echo "$TYPED" | tr -d '\r')"

[ "$E2E_FAILED" -eq 0 ] && echo "M8 typed E2E: PASS (typed values agree with A4H's stored IDoc)" || { echo "M8 typed E2E: FAIL"; exit 1; }
