#!/usr/bin/env bash
# E2E (DoD, real system): erpl_idoc renders namespaced names ('/' -> '_-') in IDoc-XML exactly
# as SAP's own XML serialization does, and a real namespaced basic type's dictionary loads.
#   1. an ABAP probe (test/e2e/abap/zcl_erpl_xml_probe.abap) makes A4H serialize a structure
#      named /BA1/F4_FX_CREATE01 with a namespaced component /1BS/STRUC1 (CALL TRANSFORMATION id);
#   2. erpl_idoc converts a flat IDoc with that IDOCTYP and a segment named /1BS/STRUC1 to XML;
#   3. the root and segment tags must equal SAP's;
#   4. XML -> flat must give the namespaced flat file back byte-exact.
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight
command -v docker >/dev/null || e2e_skip "docker not available"
command -v uvx   >/dev/null || e2e_skip "uvx (erpl-adt) not available"
docker ps --format '{{.Names}}' | grep -qx a4h || e2e_skip "a4h container not running"

echo "== M12: namespaced IDoc-XML names vs SAP's own rendering =="

PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

# 1) SAP's rendering of the names
export SAP_PASSWORD="$ERPL_SAP_PASSWORD"
ADT(){ timeout 120 uvx erpl-adt --host "$ERPL_SAP_ASHOST" --port 50000 --user "$ERPL_SAP_USER" \
        --client "$ERPL_SAP_CLIENT" --password-env SAP_PASSWORD "$@"; }
ADT object create --type CLAS/OC --name ZCL_ERPL_XML_PROBE --package '$TMP' --description 'erpl_idoc xml probe' >/dev/null 2>&1
ADT source write ZCL_ERPL_XML_PROBE --type CLAS --file test/e2e/abap/zcl_erpl_xml_probe.abap --activate >/dev/null 2>&1
SAP_XML=$(ADT object run ZCL_ERPL_XML_PROBE 2>/dev/null | tr -d '\r')
SAP_ROOT=$(echo "$SAP_XML" | grep -o '<asx:values><[^>]*>' | sed 's/<asx:values>//')
SAP_SEG=$(echo "$SAP_XML" | grep -o '<_-1BS_-STRUC1>' | head -1)
e2e_assert_eq "SAP renders /BA1/F4_FX_CREATE01 as" "<_-BA1_-F4_FX_CREATE01>" "$SAP_ROOT"
e2e_assert_eq "SAP renders /1BS/STRUC1 as"         "<_-1BS_-STRUC1>"        "$SAP_SEG"

# 2) erpl_idoc: flat IDoc with that IDOCTYP and a segment named /1BS/STRUC1
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
OUT=$("$DUCKDB" -unsigned -csv -noheader 2>/dev/null <<SQL
LOAD '$ERPL_IDOC_EXTENSION';
COPY (SELECT CASE WHEN record_type = 'C'
                  THEN replace(raw_record::VARCHAR, 'FLIGHTBOOKING_CREATEFROMDAT01', rpad('/BA1/F4_FX_CREATE01', 29, ' '))::BLOB
                  ELSE replace(raw_record::VARCHAR, 'E1SBO_CRE  ', '/1BS/STRUC1')::BLOB END
      FROM sap_idoc_read_raw('test/fixtures/flight.idoc') ORDER BY record_index)
  TO '$WORK/ns.idoc' (FORMAT sap_idoc);
COPY (SELECT * REPLACE (CASE WHEN segnam = 'E1SBO_CRE' THEN '/1BS/STRUC1' ELSE segnam END AS segnam)
      FROM read_csv('test/fixtures/flight_dict.csv')) TO '$WORK/ns_dict.csv' (FORMAT csv, HEADER true);
COPY (SELECT xml FROM sap_idoc_to_xml('$WORK/ns.idoc','$WORK/ns_dict.csv'))
  TO '$WORK/ns.xml' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '');
COPY (SELECT raw_record FROM sap_idoc_xml_to_records('$WORK/ns.xml','$WORK/ns_dict.csv') ORDER BY record_index)
  TO '$WORK/back.idoc' (FORMAT sap_idoc);
SELECT 1;
SQL
)
OUR_ROOT=$(sed -n '2p' "$WORK/ns.xml" | tr -d ' ')
OUR_SEG=$(grep -o '<_-1BS_-STRUC1' "$WORK/ns.xml" | head -1)
e2e_assert_eq "erpl_idoc root tag equals SAP's"    "$SAP_ROOT"            "$OUR_ROOT"
e2e_assert_eq "erpl_idoc segment tag equals SAP's" "${SAP_SEG%>}"         "$OUR_SEG"
cmp -s "$WORK/ns.idoc" "$WORK/back.idoc" && echo "  ok: namespaced XML -> flat is byte-exact" || { echo "  FAIL: round trip differs"; E2E_FAILED=1; }

# 3) a real namespaced basic type's dictionary loads (online, via erpl_rfc)
ROWS=$(e2e_run_sql <<'SQL'
SELECT count(*) FROM sap_idoc_dictionary(sap_idoc_params('/BA1/F4_FX_CREATE01'));
SQL
)
N=$(echo "$ROWS" | grep -E '^│ +[0-9]+ +│$' | tr -dc '0-9')
[ "${N:-0}" -gt 0 ] \
  && echo "  ok: dictionary of namespaced type /BA1/F4_FX_CREATE01 loaded from A4H" \
  || { echo "  FAIL: dictionary for /BA1/F4_FX_CREATE01 did not load: $ROWS"; E2E_FAILED=1; }

[ "$E2E_FAILED" -eq 0 ] && echo "M12 XML names E2E: PASS (erpl_idoc matches SAP's namespaced element names)" || { echo "M12 XML names E2E: FAIL"; exit 1; }
