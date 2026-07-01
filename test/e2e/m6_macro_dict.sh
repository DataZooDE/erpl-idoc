#!/usr/bin/env bash
# E2E: the extension-registered macros sap_idoc_dictionary(sap_idoc_params(...))
# fetch a dictionary from live A4H that matches the committed offline one — the
# turn-key "simplify the rfc invoke" path, entirely in SQL.
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight

echo "== sap_idoc_dictionary macro: live == committed offline dictionary =="

PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

# one-liner via the registered macros (no hand-written sap_rfc_invoke / UNNEST)
ONLINE=$(e2e_run_sql <<'SQL'
.mode csv
.header off
SELECT segnam, field_name, "offset", length
FROM sap_idoc_dictionary(sap_idoc_params('FLIGHTBOOKING_CREATEFROMDAT01'))
WHERE segnam='E1BPSBONEW' ORDER BY field_pos;
SQL
)
ONLINE=$(echo "$ONLINE" | tr -d '\r')

OFFLINE=$(e2e_run_sql <<'SQL'
.mode csv
.header off
SELECT segnam, field_name, "offset", length
FROM read_csv('test/fixtures/flight_dict.csv')
WHERE segnam='E1BPSBONEW' ORDER BY field_pos;
SQL
)
OFFLINE=$(echo "$OFFLINE" | tr -d '\r')

e2e_assert_eq "macro dictionary == committed dictionary (E1BPSBONEW)" "$OFFLINE" "$ONLINE"

# and it can be exported to a file in one COPY (Host-A workflow)
e2e_run_sql <<'SQL' >/dev/null 2>&1
COPY (SELECT * FROM sap_idoc_dictionary(sap_idoc_params('FLIGHTBOOKING_CREATEFROMDAT01')))
     TO '/tmp/erpl_idoc_macro_dict.parquet' (FORMAT parquet);
SQL
[ -s /tmp/erpl_idoc_macro_dict.parquet ] && echo "  ok: COPY (macro) TO parquet produced a file" || { echo "  FAIL: COPY export"; E2E_FAILED=1; }
rm -f /tmp/erpl_idoc_macro_dict.parquet

[ "$E2E_FAILED" -eq 0 ] && echo "macro E2E: PASS" || { echo "macro E2E: FAIL"; exit 1; }
