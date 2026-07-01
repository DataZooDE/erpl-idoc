#!/usr/bin/env bash
# M3 E2E: typed read of flight.idoc yields identical values whether the dictionary
# was fetched LIVE via erpl_rfc or read from the OFFLINE CSV (SPEC acceptance #3).
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight

echo "== M3: typed read — online dict == offline dict =="

PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

# 1) Materialize the ONLINE dictionary to a temp file (the FR-D5 bridge), then
#    typed-read using it. 2) Typed-read using the committed OFFLINE CSV.
#    Both must produce the same row.
ONLINE=$(e2e_run_sql <<'SQL'
.mode csv
.header off
COPY (
  WITH raw AS (
    SELECT PE_HEADER, PT_FIELDS FROM sap_rfc_invoke('IDOCTYPE_READ_COMPLETE',
        {'PI_IDOCTYP':'FLIGHTBOOKING_CREATEFROMDAT01','PI_CIMTYP':'','PI_VERSION':'4'})
  )
  SELECT 'FLIGHTBOOKING_CREATEFROMDAT01' AS idoctyp, '' AS cimtyp,
         (SELECT PE_HEADER.RELEASED FROM raw) AS release,
         f.SEGMENTTYP AS segnam, '' AS segdef,
         CAST(f.FIELD_POS AS INTEGER) AS field_pos, f.FIELDNAME AS field_name,
         CAST(f.BYTE_FIRST AS INTEGER) AS "offset", CAST(f.EXTLEN AS INTEGER) AS length,
         f.DATATYPE AS datatype, f.ROLLNAME AS data_element, f.DESCRP AS description,
         false AS mandatory
  FROM raw, UNNEST(PT_FIELDS) AS t(f) ORDER BY segnam, field_pos
) TO '__e2e_online_dict.csv' (FORMAT csv, HEADER true);

SELECT airlineid, connectid, flightdate, customerid, class, passname, passform, passbirth
FROM read_idoc_segment('test/fixtures/flight.idoc', 'E1BPSBONEW', '__e2e_online_dict.csv');
SQL
)
ONLINE=$(echo "$ONLINE" | tr -d '\r')

OFFLINE=$(e2e_run_sql <<'SQL'
.mode csv
.header off
SELECT airlineid, connectid, flightdate, customerid, class, passname, passform, passbirth
FROM read_idoc_segment('test/fixtures/flight.idoc', 'E1BPSBONEW', 'test/fixtures/flight_dict.csv');
SQL
)
OFFLINE=$(echo "$OFFLINE" | tr -d '\r')

rm -f __e2e_online_dict.csv

e2e_assert_eq "typed read via online dict == via offline dict" "$OFFLINE" "$ONLINE"
e2e_assert_eq "typed values (acceptance #3)" "LH,0400,20260715,00000042,Y,MUELLER,Mr,19800101" "$ONLINE"

[ "$E2E_FAILED" -eq 0 ] && echo "M3 E2E: PASS" || { echo "M3 E2E: FAIL"; exit 1; }
