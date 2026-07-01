#!/usr/bin/env bash
# M4 E2E (the DoD gate, SPEC acceptance #4): a typed IDoc composed and written by
# erpl_idoc is ACCEPTED by A4H inbound and its segments are stored — proven on the
# real system, no mocks.
#
# Flow (erpl_idoc writes; erpl_rfc + a thin ABAP importer verify — erpl_idoc never
# does RFC): compose+write with the typed-write recipe -> push file into the a4h
# container -> deploy+run an ABAP class that IDOC_INBOUND_WRITE_TO_DB's the file and
# reads the stored segment back (SAP truth) -> independently confirm the new IDoc
# via erpl_rfc sap_read_table(EDIDC).
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight
command -v docker >/dev/null || e2e_skip "docker not available"
command -v uvx   >/dev/null || e2e_skip "uvx (erpl-adt) not available"
docker ps --format '{{.Names}}' | grep -qx a4h || e2e_skip "a4h container not running"

echo "== M4: typed write -> A4H inbound acceptance =="

PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

HOST_FILE=/tmp/erpl_idoc_e2e.idoc

# 1) Compose + write a typed IDoc using ONLY erpl_idoc (typed-write recipe).
e2e_run_sql <<SQL >/dev/null
.output /dev/null
CREATE TEMP TABLE dict AS SELECT * FROM read_csv('test/fixtures/flight_dict.csv');
CREATE TEMP TABLE idoc_seg_input AS SELECT * FROM (VALUES (1,'E1SBO_CRE',1),(2,'E1BPSBONEW',2)) t(ord,segnam,hlevel);
CREATE TEMP TABLE idoc_field_values AS SELECT * FROM (VALUES
  ('E1BPSBONEW','AIRLINEID','LH'),('E1BPSBONEW','CONNECTID','0400'),('E1BPSBONEW','FLIGHTDATE','20260715'),
  ('E1BPSBONEW','CUSTOMERID','00000042'),('E1BPSBONEW','CLASS','Y'),('E1BPSBONEW','PASSNAME','MUELLER'),
  ('E1BPSBONEW','PASSFORM','Mr'),('E1BPSBONEW','PASSBIRTH','19800101')) t(segnam,field_name,value);
CREATE OR REPLACE TEMP TABLE idoc_seg_num AS SELECT ord,segnam,hlevel,row_number() OVER (ORDER BY ord) AS segnum FROM idoc_seg_input;
CREATE OR REPLACE TEMP TABLE idoc_seg_hier AS SELECT s.ord,s.segnam,s.hlevel,s.segnum, COALESCE(MAX(p.segnum),0) AS psgnum FROM idoc_seg_num s LEFT JOIN idoc_seg_num p ON p.hlevel=s.hlevel-1 AND p.segnum<s.segnum GROUP BY s.ord,s.segnam,s.hlevel,s.segnum;
CREATE OR REPLACE TEMP TABLE idoc_sdata AS SELECT h.ord,h.segnam, COALESCE(idoc_encode_sdata(list(d."offset" ORDER BY d.field_pos), list(d.length ORDER BY d.field_pos), list(COALESCE(v.value,'') ORDER BY d.field_pos)), repeat(' ',1000)) AS sdata FROM idoc_seg_hier h LEFT JOIN dict d ON d.segnam=h.segnam LEFT JOIN idoc_field_values v ON v.segnam=h.segnam AND v.field_name=d.field_name GROUP BY h.ord,h.segnam;
.output
COPY (SELECT raw FROM (
  SELECT 0 AS ord, idoc_encode_control([tabnam,mandt,docnum,docrel,status,direct,outmod,exprss,test,idoctyp,cimtyp,mestyp,mescod,mesfct,std,stdvrs,stdmes,sndpor,sndprt,sndpfc,sndprn,sndsad,sndlad,rcvpor,rcvprt,rcvpfc,rcvprn,rcvsad,rcvlad,credat,cretim,refint,refgrp,refmes,arckey,serial]) AS raw FROM read_idoc_control('test/fixtures/flight.idoc')
  UNION ALL
  SELECT h.ord, idoc_encode_data_record(h.segnam,'001',0,h.segnum,h.psgnum,h.hlevel,sd.sdata) FROM idoc_seg_hier h JOIN idoc_sdata sd USING(ord,segnam)
) ORDER BY ord) TO '$HOST_FILE' (FORMAT idoc);
SQL
[ -f "$HOST_FILE" ] && [ "$(wc -c < "$HOST_FILE")" -eq 2650 ] || { echo "FAIL: typed write did not produce a 2650-byte file"; exit 1; }
echo "  ok: erpl_idoc wrote $(wc -c < "$HOST_FILE")-byte typed IDoc"

# 2) Push the file into the container (docker cp trips on the overlay; use stdin).
docker exec -i a4h sh -c 'cat > /tmp/erpl_idoc_e2e.idoc' < "$HOST_FILE"

# 3) Deploy + run the ABAP importer (SAP reads the flat file and persists it).
export SAP_PASSWORD="$ERPL_SAP_PASSWORD"
ADT(){ timeout 120 uvx erpl-adt --host "$ERPL_SAP_ASHOST" --port 50000 --user "$ERPL_SAP_USER" \
        --client "$ERPL_SAP_CLIENT" --password-env SAP_PASSWORD "$@"; }
ADT object create --type CLAS/OC --name ZCL_ERPL_IDOC_E2E --package '$TMP' \
        --description 'erpl_idoc E2E inbound' >/dev/null 2>&1
ADT source write ZCL_ERPL_IDOC_E2E --type CLAS --file test/e2e/abap/zcl_erpl_idoc_e2e.abap --activate >/dev/null 2>&1
RUN=$(ADT object run ZCL_ERPL_IDOC_E2E 2>/dev/null)

DOCNUM=$(echo "$RUN" | sed -n 's/^DOCNUM=//p' | tr -d '\r')
SDATA=$(echo "$RUN"  | sed -n 's/^SDATA=//p'  | tr -d '\r')
SEGCOUNT=$(echo "$RUN" | sed -n 's/^SEGCOUNT=//p' | tr -d '\r')
echo "  A4H stored docnum=$DOCNUM segcount=$SEGCOUNT sdata='$SDATA'"

[ -n "$DOCNUM" ] && [ "$DOCNUM" != "0000000000000000" ] || { echo "FAIL: no docnum from inbound"; exit 1; }
e2e_assert_eq "SAP re-read of typed segment (SDATA)"  "LH 04002026071500000042Y" "$SDATA"
e2e_assert_eq "SAP stored both segments"              "2"                        "$SEGCOUNT"

# The ABAP importer's post-commit IDOC_READ_COMPLETELY readback above is the
# authoritative SAP-truth check (same LUW, no race). The following is an additional
# SQL/RFC-layer confirmation via erpl_rfc; the update-task commit can lag a fresh RFC
# session by a few seconds, so it polls and only WARNs (never fails) on a lag.
FOUND=0
for _ in $(seq 1 15); do
	FOUND=$(e2e_run_sql <<SQL 2>/dev/null
.mode csv
.header off
SELECT count(*) FROM sap_read_table('EDIDC', COLUMNS=['DOCNUM'],
       FILTER='DOCNUM = ''$DOCNUM''');
SQL
)
	FOUND=$(echo "$FOUND" | tr -d '\r' | grep -oE '[0-9]+' | head -1)
	[ "$FOUND" == "1" ] && break
	sleep 2
done
if [ "$FOUND" == "1" ]; then
	echo "  ok: erpl_rfc also sees the new IDoc in EDIDC"
else
	echo "  warn: erpl_rfc EDIDC read lagged (commit visibility) — ABAP readback already confirmed storage"
fi

[ "$E2E_FAILED" -eq 0 ] && echo "M4 E2E: PASS (A4H accepted the erpl_idoc-written IDoc)" || { echo "M4 E2E: FAIL"; exit 1; }
