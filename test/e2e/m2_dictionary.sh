#!/usr/bin/env bash
# M2 E2E: the ONLINE dictionary (live erpl_rfc → IDOCTYPE_READ_COMPLETE) is
# identical to the persisted OFFLINE dictionary CSV for the slicing columns that
# drive typed access (SPEC acceptance #3 symmetry, FR-D1).
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight

echo "== M2: online vs offline dictionary symmetry =="

# ping first — skip cleanly if A4H is down
PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

# online dictionary for E1BPSBONEW, projected to the slicing tuple.
# Uses the documented recipe (sql/sap_idoc_dictionary.sql) with its <<<...>>>
# literals substituted — this is exactly how a user runs the online path.
ONLINE=$(e2e_run_sql <<'SQL'
.mode csv
.header off
WITH raw AS (
    SELECT PT_FIELDS FROM sap_rfc_invoke('IDOCTYPE_READ_COMPLETE',
        {'PI_IDOCTYP':'FLIGHTBOOKING_CREATEFROMDAT01','PI_CIMTYP':'','PI_VERSION':'4'})
)
SELECT f.SEGMENTTYP, f.FIELDNAME, CAST(f.BYTE_FIRST AS INTEGER), CAST(f.EXTLEN AS INTEGER)
FROM raw, UNNEST(PT_FIELDS) AS t(f)
WHERE f.SEGMENTTYP='E1BPSBONEW'
ORDER BY CAST(f.FIELD_POS AS INTEGER);
SQL
)

# offline dictionary (committed CSV), same projection
OFFLINE=$(e2e_run_sql <<'SQL'
.mode csv
.header off
SELECT segnam, field_name, "offset", length
FROM read_csv('test/fixtures/flight_dict.csv')
WHERE segnam='E1BPSBONEW' ORDER BY field_pos;
SQL
)

e2e_assert_eq "online dictionary == offline dictionary (E1BPSBONEW slicing)" "$OFFLINE" "$ONLINE"

# spot-check a couple of exact values
FLIGHTDATE=$(echo "$ONLINE" | grep FLIGHTDATE | tr -d '\r')
e2e_assert_eq "online FLIGHTDATE offset/length" "E1BPSBONEW,FLIGHTDATE,7,8" "$FLIGHTDATE"

[ "$E2E_FAILED" -eq 0 ] && echo "M2 E2E: PASS" || { echo "M2 E2E: FAIL"; exit 1; }
