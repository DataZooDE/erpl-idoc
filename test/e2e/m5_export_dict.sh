#!/usr/bin/env bash
# E2E: the export helper (scripts/export_idoc_dict.sh) produces, from the live A4H
# system, a dictionary file identical to the committed offline one — proving the
# "create dictionary files" path end to end (FR-D4/FR-D5).
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight
command -v uvx >/dev/null 2>&1 || true

echo "== export_idoc_dict.sh: live export == committed offline dictionary =="

PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

OUT=/tmp/erpl_idoc_export_test.csv
rm -f "$OUT"
bash "$REPO_ROOT/scripts/export_idoc_dict.sh" FLIGHTBOOKING_CREATEFROMDAT01 --format csv --out "$OUT" >/dev/null 2>&1
[ -s "$OUT" ] || { echo "FAIL: export produced no file"; exit 1; }

# compare the slicing columns (segnam, field_name, offset, length) to the fixture
EXPORTED=$(e2e_run_sql <<SQL 2>/dev/null
.mode csv
.header off
SELECT segnam, field_name, "offset", length FROM read_csv('$OUT') ORDER BY segnam, field_pos;
SQL
)
COMMITTED=$(e2e_run_sql <<'SQL' 2>/dev/null
.mode csv
.header off
SELECT segnam, field_name, "offset", length FROM read_csv('test/fixtures/flight_dict.csv') ORDER BY segnam, field_pos;
SQL
)
rm -f "$OUT"

e2e_assert_eq "exported dictionary == committed dictionary" "$COMMITTED" "$EXPORTED"

[ "$E2E_FAILED" -eq 0 ] && echo "M5 export E2E: PASS" || { echo "M5 export E2E: FAIL"; exit 1; }
