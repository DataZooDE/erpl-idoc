#!/usr/bin/env bash
# E2E (DoD, real system): how A4H interprets non-ASCII bytes of a flat IDoc, and that
# erpl_idoc's `encoding` option decodes the same bytes to the same characters.
#
# A flat IDoc file is a byte stream; SAP reads it with OPEN DATASET ... IN LEGACY BINARY
# MODE (one byte -> one character, via the system's legacy code page). We write PASSNAME
# with the bytes 0xDC 0x80 0xE4 0x9F, let A4H import it (IDOC_INBOUND_WRITE_TO_DB), read
# back what SAP stored (UTF-16BE hex), and require erpl_idoc to decode the identical file
# to the identical characters. 0x80 / 0x9F separate cp1252 (EUR / Y-diaeresis) from
# latin-1 (C1 control characters).
source "$(dirname "${BASH_SOURCE[0]}")/e2e_common.sh"
e2e_preflight
command -v docker >/dev/null || e2e_skip "docker not available"
command -v uvx   >/dev/null || e2e_skip "uvx (erpl-adt) not available"
command -v python3 >/dev/null || e2e_skip "python3 not available"
docker ps --format '{{.Names}}' | grep -qx a4h || e2e_skip "a4h container not running"

echo "== M8: IDoc encoding — A4H (legacy binary mode) vs erpl_idoc encoding option =="

PING=$(e2e_run_sql <<'SQL'
PRAGMA sap_rfc_ping;
SQL
)
echo "$PING" | grep -q PONG || e2e_skip "A4H not reachable (ping != PONG)"

HOST_FILE=/tmp/erpl_idoc_enc.idoc
BYTES=${ENC_PASSNAME_BYTES:-dc80e49f}

# 1) flight.idoc with PASSNAME (E1BPSBONEW data record, SDATA offset 40, 25 bytes) replaced.
BYTES="$BYTES" HOST_FILE="$HOST_FILE" python3 - <<'PY'
import os
b = bytearray(open('test/fixtures/flight.idoc', 'rb').read())
name = bytes.fromhex(os.environ['BYTES'])
assert len(name) <= 25
pos = 524 + 1063 + 63 + 40          # control rec + 1st data rec + header + SDATA offset
b[pos:pos + 25] = name.ljust(25, b' ')
open(os.environ['HOST_FILE'], 'wb').write(b)
PY

# 2) Import into A4H and read back what SAP stored.
docker exec -i a4h sh -c 'cat > /tmp/erpl_idoc_enc.idoc' < "$HOST_FILE"
export SAP_PASSWORD="$ERPL_SAP_PASSWORD"
ADT(){ timeout 120 uvx erpl-adt --host "$ERPL_SAP_ASHOST" --port 50000 --user "$ERPL_SAP_USER" \
        --client "$ERPL_SAP_CLIENT" --password-env SAP_PASSWORD "$@"; }
ADT object create --type CLAS/OC --name ZCL_ERPL_IDOC_ENC --package '$TMP' --description 'erpl_idoc E2E encoding' >/dev/null 2>&1
ADT source write ZCL_ERPL_IDOC_ENC --type CLAS --file test/e2e/abap/zcl_erpl_idoc_enc.abap --activate >/dev/null 2>&1
RUN=$(ADT object run ZCL_ERPL_IDOC_ENC 2>/dev/null)
SAP_HEX=$(echo "$RUN" | sed -n 's/^PASSNAME_HEX=//p' | tr -d '\r' | tr 'A-F' 'a-f')
echo "  A4H stored PASSNAME as UTF-16BE: ${SAP_HEX:0:24}…"
[ -n "$SAP_HEX" ] || { echo "FAIL: no PASSNAME read back from A4H"; echo "$RUN"; exit 1; }

# 3) erpl_idoc decodes the same file with encoding='latin-1' (what SAP's legacy binary mode
#    does); compare code points (4 hex digits per character, like UTF-16BE) with SAP's.
ENC=${ENC_OPTION:-latin-1}
ERPL_HEX=$("$DUCKDB" -unsigned -noheader -list 2>&1 <<SQL | tail -1 | sed 's/\x1b\[[0-9;]*m//g'
LOAD '$ERPL_IDOC_EXTENSION';
SELECT array_to_string(list_transform(string_split(substr(passname, 1, 4), ''), x -> printf('%04x', unicode(x))), '')
FROM sap_idoc_read_segment('$HOST_FILE','E1BPSBONEW','test/fixtures/flight_dict.csv', encoding='$ENC');
SQL
)
echo "  erpl_idoc ($ENC) PASSNAME code points: $ERPL_HEX"
e2e_assert_eq "erpl_idoc decodes the bytes to the characters SAP stored" "${SAP_HEX:0:16}" "$ERPL_HEX"

[ "$E2E_FAILED" -eq 0 ] && echo "M8 encoding E2E: PASS" || { echo "M8 encoding E2E: FAIL"; exit 1; }
