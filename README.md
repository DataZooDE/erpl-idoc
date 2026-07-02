<a name="top"></a>

[![License: BSL 1.1](https://img.shields.io/badge/License-BSL%201.1-blue.svg)](LICENSE)
[![DuckDB](https://img.shields.io/badge/DuckDB-1.5.4+-green.svg)](https://duckdb.org)
[![Community Extension](https://img.shields.io/badge/DuckDB-Community%20Extension-informational.svg)](https://duckdb.org/community_extensions/)
[![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)]()

# ERPL IDoc — Read & Write SAP IDoc files in DuckDB

**Turn SAP IDoc files into SQL tables — and SQL back into byte-valid IDoc files.**
`erpl_idoc` is a DuckDB extension for the SAP **IDoc** (Intermediate Document) format:
the fixed-width flat files and the self-describing IDoc-XML that ALE/EDI interfaces
exchange every day. Parse them, decode the opaque `SDATA` into typed columns, generate
new IDocs from a query, and convert flat ⇄ XML — all in plain SQL, all offline. It’s the
document/EDI layer of the **erpl** SAP family (`erpl_rfc`, `erpl_odp`, `erpl_bics`), and
composes with `erpl_rfc` when you want live-SAP round trips.

<p align="center">
  <img src="assets/erpl_idoc_demo.gif" alt="erpl_idoc demo: read an IDoc file, decode SDATA into typed columns, write a byte-exact IDoc back, and convert flat to IDoc-XML — all in DuckDB SQL" width="820">
</p>

> SEO topics: DuckDB SAP IDoc, read IDoc file SQL, parse EDI_DC40 EDI_DD40, IDoc flat file
> to table, IDoc XML to flat, generate IDoc from SQL, SAP ALE EDI DuckDB, decode SDATA,
> segment dictionary WE60, IDOCTYPE_READ_COMPLETE, IDoc round trip.

## ✨ Highlights

- **Read any IDoc file as a table** — one `SELECT` over a flat IDoc or IDoc-XML file.
- **Typed decode** — split the opaque 1000-char `SDATA` into named, typed columns via a
  segment dictionary. The control record (`EDI_DC40`, all 36 fields) reads typed too.
- **Write byte-valid IDocs from SQL** — `COPY (…) TO 'x.idoc' (FORMAT sap_idoc)`; the
  writer recomputes derived fields (`SEGNUM`, `PSGNUM`, `HLEVEL`, lengths).
- **Flat ⇄ IDoc-XML conversion** — modernize a flat interface to XML or vice versa,
  losslessly.
- **Byte-exact round trips** — `read → write` reproduces the input file bit-for-bit.
- **Offline & portable** — the core needs no SAP and no network; works on a detached,
  air-gapped host. Linux, macOS, and Windows.
- **Framing & encoding** — contiguous fixed-width or LF/CRLF ports (auto-detected);
  UTF-8 and `latin-1` codepages; lenient mode for truncated files.
- **Composes with `erpl_rfc`** — fetch the dictionary from a live system, or import a
  generated IDoc, using documented SQL — but `erpl_idoc` itself never speaks RFC.

---

## 🚀 Install

```sql
INSTALL erpl_idoc FROM community;
LOAD erpl_idoc;
```

That’s it — no SAP connection required to read, write, or convert IDoc files.

---

## ⚡ Quick Start

### Read an IDoc file

```sql
-- one row per data record: segment name, hierarchy, and the raw SDATA payload
SELECT segnam, hlevel, sdata
FROM sap_idoc_read('orders.idoc');

-- the envelope (control record) as 36 typed columns
SELECT idoctyp, mestyp, sndprn, rcvprn, credat
FROM sap_idoc_read_control('orders.idoc');
```

### Decode SDATA into typed columns

`SDATA` is opaque fixed-width until you apply a **segment dictionary**. Point at one
(a CSV/Parquet file, a table, or a view) and get named columns:

```sql
SELECT airlineid, flightdate, customerid, class, passname
FROM sap_idoc_read_segment('booking.idoc', 'E1BPSBONEW', 'flightbooking.dict.parquet');
-- LH | 20260715 | 00000042 | Y | MUELLER
```

### Generate an IDoc file from SQL

Compose records from your data and write a byte-valid IDoc:

```sql
COPY (
  SELECT raw_record
  FROM sap_idoc_read_raw('template.idoc')   -- or build records with the encoders
  ORDER BY record_index
) TO 'outbound.idoc' (FORMAT sap_idoc);
```

### Convert flat ⇄ IDoc-XML

```sql
-- flat  → self-describing XML
SELECT xml FROM sap_idoc_to_xml('orders.idoc', 'orders.dict.parquet');

-- XML → flat (write it out)
COPY (SELECT raw_record FROM sap_idoc_xml_to_records('orders.xml','orders.dict.parquet') ORDER BY record_index)
  TO 'orders.idoc' (FORMAT sap_idoc);
```

---

## 💡 Use cases

### 1. Land inbound IDocs in your warehouse
A partner drops `ORDERS05`/`INVOIC02`/`MATMAS05` files on a landing zone. Query them
straight into DuckDB for staging, validation, and analytics — no middleware:

```sql
CREATE TABLE staged_orders AS
SELECT document_key, hdr.*
FROM sap_idoc_read_segment('inbox/po_4711.idoc', 'E1EDK01', 'orders.dict.parquet') hdr;
```

### 2. Decode & reconcile opaque payloads
SAP/ALE consultants: split `SDATA` into fields and reconcile values against the segment
definition, or diff two IDocs field-by-field:

```sql
SELECT segnam, field_name, value
FROM sap_idoc_read_xml('doc.xml')            -- self-describing, no dictionary needed
WHERE value <> '';
```

### 3. Produce outbound IDocs from transformed data
Build IDoc content in SQL (joins, lookups, mappings) and emit a file a SAP file-port
or inbound processing accepts. The writer handles fixed-width packing and derived-field
math for you.

### 4. Migrate a flat-file interface to IDoc-XML (or back)
Two systems, two serializations. Convert losslessly in one step — `flat → xml → flat`
is byte-exact — so you can switch a port’s format without touching the payload.

### 5. Typed decode on an air-gapped host
Fetch the segment dictionary **once** from a connected system, persist it to Parquet,
then decode IDocs on a detached host with **no SAP and no `erpl_rfc`**:

```sql
-- on a SAP-less machine — only erpl_idoc + the dictionary file
SELECT * FROM sap_idoc_read_segment('doc.idoc', 'E1BPSBONEW', 'flightbooking.dict.parquet');
```

### 6. Validate before you send
Catch structural problems early — bad offsets, overlaps, missing fields — and keep a
byte-exact round trip as your safety net:

```sql
SELECT * FROM sap_idoc_dict_validate('mytype.dict.csv');   -- empty result = sound
```

---

## 📖 Function reference

### Reading

| Function | What you get |
|---|---|
| `sap_idoc_read(path [, framing, lenient, encoding])` | generic long rows: `document_key, docnum, segnum, segnam, psgnum, hlevel, mandt, sdata` |
| `sap_idoc_read_control(path [, …])` | the control record — all 36 `EDI_DC40` fields, typed (flat **or** XML) |
| `sap_idoc_read_segment(path, segnam, dict [, …])` | typed columns for one segment type, sliced from `SDATA` per the dictionary |
| `sap_idoc_read_fields(path, dict [, …])` | **every field of every record** in one call — long rows: `document_key, segnum, psgnum, hlevel, segnam, field_pos, field_name, datatype, value` |
| `sap_idoc_read_raw(path [, …])` | one row per physical record with exact bytes — the byte-exact writer source |
| `sap_idoc_read_xml(path)` | generic long rows from an IDoc-XML file (self-describing; no dictionary) |

Every reader accepts a **single path, a glob, or a `LIST` of paths**, resolved through
DuckDB's virtual filesystem — so a whole directory works, including remote stores
(`s3://…`, `http(s)://…`, `gs://…`) once the matching extension is loaded
(`INSTALL httpfs; LOAD httpfs;`) and a `CREATE SECRET` is set for credentials:

```sql
SELECT * FROM sap_idoc_read(['a.idoc', 'b.idoc']);          -- explicit list
```


```sql
SELECT filename, idoctyp FROM sap_idoc_read_control('s3://bucket/idocs/*.idoc', filename=true);
```

Parameters (all readers): `framing` = `'fixed'` (default) \| `'lf'` \| `'crlf'`
(auto-detected when omitted) · `lenient := true` salvages complete records from a
truncated file · `encoding` = `'utf-8'` (default) \| `'latin-1'` · `filename := true`
adds a source-file column (handy across a glob). `sap_idoc_read_fields` also takes
`include_unknown := false` to drop segments absent from the dictionary (default keeps
them as one row with the raw trimmed `SDATA`).

The readers are **streaming and parallel**: each file is parsed record-by-record in
constant memory (never fully buffered), and a glob/`LIST` is read with one thread per
file. Rows are therefore **unordered across files** (order within a file is preserved) —
add `ORDER BY` if you need a stable order, exactly as with `read_csv`/`read_parquet`.

```sql
-- Decode a whole IDoc — all segments, all fields — in one call:
SELECT segnam, field_name, value
FROM sap_idoc_read_fields('order.idoc', 'order_dict.csv');
```

### Writing

```sql
COPY (<single BLOB/VARCHAR column of raw records>)
  TO 'file.idoc' (FORMAT sap_idoc [, framing 'fixed'|'lf'|'crlf', validate true]);
```

Build the records with the pure encoders when composing from scratch:

| Encoder | Produces |
|---|---|
| `sap_idoc_encode_sdata(offsets, lengths, values)` | a 1000-byte `SDATA` payload |
| `sap_idoc_encode_data_record(segnam, mandt, docnum, segnum, psgnum, hlevel, sdata)` | a 1063-byte `EDI_DD40` record |
| `sap_idoc_encode_control(values)` | a 524-byte `EDI_DC40` control record |

### Converting (flat ⇄ XML)

| Function | Direction |
|---|---|
| `sap_idoc_to_xml(flat_path, dict)` | flat → IDoc-XML text |
| `sap_idoc_xml_to_records(xml_path, dict)` | IDoc-XML → flat records (for `COPY … (FORMAT sap_idoc)`) |

### Dictionary tooling

| Function | Purpose |
|---|---|
| `sap_idoc_dict_offsets(src)` | compute field offsets from lengths (author a dict from field order + width) |
| `sap_idoc_dict_validate(src)` | list structural problems; empty = sound |
| `sap_idoc_dict_from_fields(fields, idoctyp, cimtyp, release)` | normalize a raw `IDOCTYPE_READ_COMPLETE` field list to the dictionary schema |

Every function is self-documenting — `SELECT * FROM duckdb_functions() WHERE function_name LIKE 'sap_idoc_%'`
shows a description and an example for each.

---

## 🔤 The segment dictionary

Typed mode needs to know each segment’s field layout (name, offset, length, type). That
“segment dictionary” is just a **relation** with these columns:

```
idoctyp, cimtyp, release, segnam, segdef, field_pos, field_name, offset, length, datatype, ...
```

Its origin is irrelevant to the parser — a file, a table, a view, or a query all work:

- **Offline / hand-authored** — write the fields (order + width) and let
  `sap_idoc_dict_offsets` compute the offsets; check it with `sap_idoc_dict_validate`.
- **Online, from a live system** (requires `erpl_rfc` loaded) — the extension ships two
  SQL macros so it’s a one-liner:

  ```sql
  LOAD erpl_rfc; LOAD erpl_idoc;
  CREATE SECRET sap (TYPE sap_rfc, ASHOST '…', SYSNR '00', CLIENT '100', USER '…', PASSWD '…');

  -- fetch + normalize the dictionary for a basic type
  SELECT * FROM sap_idoc_dictionary(sap_idoc_params('ORDERS05'));

  -- persist once → reuse forever offline (the connected → detached bridge)
  COPY (SELECT * FROM sap_idoc_dictionary(sap_idoc_params('ORDERS05')))
       TO 'orders.dict.parquet' (FORMAT parquet);
  ```

---

## 🔁 Round-trip guarantees

- **Generic:** `sap_idoc_read_raw → COPY (FORMAT sap_idoc)` reproduces the input file
  **byte-for-byte**.
- **Flat ⇄ XML:** `flat → xml → flat` (and `xml → flat`) is byte-exact with the dictionary.
- **System-true:** IDocs written/converted by `erpl_idoc` are accepted by SAP inbound
  processing (validated end-to-end against a live SAP AS ABAP system, not mocks).

---

## 🔌 Live SAP, cleanly separated

`erpl_idoc` is a **pure, offline file engine** — it links no SAP libraries and makes no
network calls. When you want a live round trip, you *compose* it with
[`erpl_rfc`](https://github.com/DataZooDE/erpl) in SQL:

- **Get a dictionary** — `sap_idoc_dictionary(sap_idoc_params('ORDERS05'))` (above).
- **Import a generated IDoc** — hand the records to an `erpl_rfc` inbound call.

This keeps the file format portable and dependency-free while still giving you the full
loop when a system is reachable.

---

## 🧭 Scope

**In scope:** flat `EDI_DC40`/`EDI_DD40`, generic + typed read/write, the full control
record, the segment dictionary (online/offline), both framings, multi-IDoc files,
lenient/error handling, `latin-1`/UTF-8 decode, and IDoc-XML read/write + flat↔XML
conversion.

**Not (yet):** `EDIDS` status-record side output, X12/EDIFACT conversion, and
business-semantic validation (only structural validity is checked).

---

## 🛠️ Build from source

```sh
git clone --recurse-submodules https://github.com/DataZooDE/erpl-idoc.git
cd erpl-idoc
make debug          # or: make release
make test           # SQL test suite
```

`tinyxml2` (IDoc-XML) is pulled via vcpkg; set `VCPKG_ROOT` before building.

## 🤝 Contributing

Issues and PRs welcome. The engineering norm here is **TDD with no mocks** — “done”
means it works end-to-end against a real SAP system, verified by a byte-exact round trip.

## 🔐 Telemetry

`erpl_idoc` collects **anonymous, opt-out** usage telemetry (extension/DuckDB version,
OS/arch, and which functions are invoked) so we know what to keep working. **No IDoc
content, file paths, connection details, or personal data are ever collected.** Disable
it at any time:

```sql
SET erpl_telemetry_enabled = FALSE;   -- turn telemetry off
SET erpl_telemetry_key = 'your-key';  -- or point it at your own PostHog project
```

Same mechanism as the rest of the [erpl](https://github.com/DataZooDE/erpl) family —
see [erpl.io/telemetry](https://erpl.io/telemetry) for details.

## 📄 License

[Business Source License 1.1](LICENSE) (Licensor: DataZoo GmbH; Change License MPL 2.0
after 5 years) — the same license as the rest of the [erpl](https://github.com/DataZooDE/erpl)
family. Non-production use is free; production use is granted except offering it to third
parties on a hosted or embedded basis. Part of the erpl family by DataZoo.

<sub>[⬆ back to top](#top)</sub>
