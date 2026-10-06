<a name="top"></a>

[![License: BSL 1.1](https://img.shields.io/badge/License-BSL%201.1-blue.svg)](LICENSE)
[![DuckDB](https://img.shields.io/badge/DuckDB-1.4.5%20%7C%201.5.6-green.svg)](https://duckdb.org)
[![Community Extension](https://img.shields.io/badge/DuckDB-Community%20Extension-informational.svg)](https://duckdb.org/community_extensions/)
[![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)]()

# ERPL IDoc — Read & Write SAP IDoc files in DuckDB

**Turn SAP IDoc files into SQL tables — and SQL back into byte-valid IDoc files.**
`erpl_idoc` is a DuckDB extension for the SAP **IDoc** (Intermediate Document) format:
the fixed-width flat files and the self-describing IDoc-XML that ALE/EDI interfaces
exchange every day. Parse them, decode the opaque `SDATA` into named columns, generate
new IDocs from a query, and convert flat ⇄ XML — all in plain SQL, all offline. It’s the
document/EDI layer of the **erpl** SAP family (`erpl_rfc`, `erpl_odp`, `erpl_bics`), and
composes with `erpl_rfc` when you want live-SAP round trips.

<p align="center">
  <img src="assets/erpl_idoc_demo.gif" alt="erpl_idoc demo: read an IDoc file, decode SDATA into named columns, write a byte-exact IDoc back, and convert flat to IDoc-XML — all in DuckDB SQL" width="820">
</p>

> SEO topics: DuckDB SAP IDoc, read IDoc file SQL, parse EDI_DC40 EDI_DD40, IDoc flat file
> to table, IDoc XML to flat, generate IDoc from SQL, SAP ALE EDI DuckDB, decode SDATA,
> segment dictionary WE60, IDOCTYPE_READ_COMPLETE, IDoc round trip.

## ✨ Highlights

- **Read any IDoc file as a table** — one `SELECT` over a flat IDoc or IDoc-XML file.
- **Dictionary decode** — split the opaque 1000-char `SDATA` into named columns via a
  segment dictionary (text by default, or real `DATE`/`TIME`/`DECIMAL` with `typed := true`).
  The control record (`EDI_DC40`, all 36 fields) reads as named text columns too.
- **Write byte-valid IDocs from SQL** — `COPY (…) TO 'x.idoc' (FORMAT sap_idoc)` writes the
  records you give it and checks their widths. The encoders pad fields to their lengths; you
  supply the hierarchy (`SEGNUM`, `PSGNUM`, `HLEVEL`) — see
  *Generate an IDoc from SQL (from scratch)*.
- **Flat ⇄ IDoc-XML conversion** — modernize a flat interface to XML or vice versa. Exact
  for IDocs whose segments are fully described by the dictionary (see
  *Round-trip guarantees* below).
- **Byte-exact round trips** — `sap_idoc_read_raw → write` reproduces the input file bit-for-bit.
- **Offline & portable** — the core needs no SAP and no network; works on a detached,
  air-gapped host. See *Platforms and DuckDB versions* below.
- **Framing & encoding** — contiguous fixed-width or LF/CRLF ports (auto-detected);
  UTF-8, ASCII, `latin-1` and `cp1252` text; lenient mode for truncated files.
- **Composes with `erpl_rfc`** — fetch the dictionary from a live system in plain SQL.
  `erpl_idoc` itself never speaks RFC (see *Live SAP, cleanly separated* below).

---

## 🚀 Install

```sql
INSTALL erpl_idoc FROM community;
LOAD erpl_idoc;
```

That’s it — no SAP connection required to read, write, or convert IDoc files.

### Platforms and DuckDB versions

CI builds and deploys the extension for **DuckDB v1.5.6** and the **v1.4.5 LTS** line on `linux_amd64`,
`linux_arm64`, `osx_amd64`, `osx_arm64` and `windows_amd64`. The release gate installs the built artifact into
the official DuckDB CLI and calls a function on `linux_amd64`, `osx_arm64` and `windows_amd64`; the other
platforms are built but not smoke-tested. WebAssembly and the `windows_amd64_mingw`/`rtools` builds are not
published. Workflow: `.github/workflows/MainDistributionPipeline.yml`. To build from source, see the section at the end.

---

## ⚡ Quick Start

### Read an IDoc file

```sql
-- one row per data record: segment name, hierarchy, and the raw SDATA payload
SELECT segnam, hlevel, sdata
FROM sap_idoc_read('orders.idoc');

-- the envelope (control record) as 36 named text columns
SELECT idoctyp, mestyp, sndprn, rcvprn, credat
FROM sap_idoc_read_control('orders.idoc');
```

### Decode SDATA into named columns

`SDATA` is opaque fixed-width until you apply a **segment dictionary**. Point at one
(a CSV/Parquet file, a table, or a view) and get named columns. Column names come out
**lower-case**; values are `VARCHAR` text unless you pass `typed := true` (below):

```sql
SELECT airlineid, flightdate, customerid, class, passname
FROM sap_idoc_read_segment('booking.idoc', 'E1BPSBONEW', 'flightbooking.dict.parquet');
-- LH | 20260715 | 00000042 | Y | MUELLER
```

#### Real SQL types: `typed := true`

By default every column is `VARCHAR` — exactly the SAP text, trailing pad trimmed. Add
`typed := true` to get SQL types for the fields where that is unambiguous:

```sql
SELECT flightdate, passbirth                       -- DATE, DATE
FROM sap_idoc_read_segment('booking.idoc', 'E1BPSBONEW', 'flightbooking.dict.parquet', typed := true);
-- 2026-07-15 | 1980-01-01
```

| Dictionary `datatype` | Column type with `typed := true` |
|---|---|
| `DATS` | `DATE` (blank and `00000000` → `NULL`; year `0000` is invalid) |
| `TIMS` | `TIME` (blank → `NULL`; `000000` is midnight, `240000` is invalid) |
| `DEC`, `CURR`, `QUAN` | `DECIMAL(max(length, decimals+1), decimals)` **only if the dictionary has a `decimals` column** with a value for the field; otherwise `VARCHAR`. Fields wider than 38 digits also stay `VARCHAR` |
| `NUMC`, `CHAR`, `LANG`, `UNIT`, `CUKY`, everything else | `VARCHAR` — `NUMC` stays text because leading zeros are significant for keys |

SAP's own field list (`IDOCTYPE_READ_COMPLETE`, and so `sap_idoc_dictionary(…)`) does **not**
report decimals, so amounts and quantities stay text until you add a `decimals` column to your
dictionary. The column name is case-insensitive and may be text in a CSV; a missing or non-numeric
entry means "not supplied" for that field:

```sql
CREATE TABLE my_dict AS
  SELECT d.*, CASE d.field_name WHEN 'MENGE' THEN 3 WHEN 'NETWR' THEN 2 END AS decimals
  FROM sap_idoc_dictionary(sap_idoc_params('ORDERS05')) d;

SELECT menge, netwr FROM sap_idoc_read_segment('orders.idoc', 'E1EDP01', 'my_dict', typed := true);
```

DECIMAL text is read **as written**: an explicit decimal point (`1234.50`), an optional sign, and SAP's
trailing minus (`5.000-`) are understood; there is no implied decimal point (`5` is `5.00`), and
exponents (`1e3`) and thousands separators (`1,234.50`) are invalid. More fractional digits than
`decimals` is invalid too — values are never rounded.

An invalid value raises an error naming the field, datatype, value, expected shape, segment, document
and file; `strict := false` reads it as `NULL` instead. Strict mode checks **every** field of each scanned
segment, not only the selected columns. `strict` (bad *values*) is unrelated to `lenient` (salvaging a
*truncated file*), and it is an error without `typed := true`.
`sap_idoc_read_fields` is unchanged — its single `value` column mixes all fields, so it stays `VARCHAR`.

Typed values write back by formatting them as SAP text — the writer takes strings:

```sql
-- DATE → 'YYYYMMDD', TIME → 'HHMMSS', DECIMAL → text; SAP puts the minus of a negative number last
strftime(flightdate, '%Y%m%d'),
replace(tim::VARCHAR, ':', ''),
CASE WHEN qty < 0 THEN abs(qty)::VARCHAR || '-' ELSE qty::VARCHAR END
```

### Generate an IDoc from SQL (from scratch)

`COPY (FORMAT sap_idoc)` writes the records you give it — one record per row, in order — and,
with `validate true` (the default), checks each record's width. It does **not** compute the
hierarchy: `SEGNUM`, `PSGNUM` and `HLEVEL` are inputs to `sap_idoc_encode_data_record`, and
row order is the order of the file, so always `ORDER BY`.

```sql
-- re-emit an existing fixed-width file (byte-exact); for an LF/CRLF file add  framing 'lf' / 'crlf'
COPY (SELECT raw_record FROM sap_idoc_read_raw('template.idoc') ORDER BY record_index)
  TO 'outbound.idoc' (FORMAT sap_idoc);

-- build a new record: reuse the control record from a template (record_type 'C' = control, 'D' = data),
-- then encode a data record
COPY (
  SELECT raw FROM (
    SELECT 0 AS ord, raw_record AS raw FROM sap_idoc_read_raw('template.idoc') WHERE record_type = 'C'
    UNION ALL
    SELECT 1, sap_idoc_encode_data_record(
                'E1SBO_CRE', '001', 1, 1, 0, 1,                      -- segnam, mandt, docnum, segnum, psgnum, hlevel
                sap_idoc_encode_sdata([0], [3], ['abc']))            -- sdata: offsets, lengths, values
  ) ORDER BY ord
) TO 'outbound.idoc' (FORMAT sap_idoc);
```

Composing from business tables means computing `SEGNUM` (`row_number()`), `PSGNUM` (the nearest
preceding segment one level up) and the `SDATA` per segment from a dictionary.
[`sql/write_idoc_typed.sql`](sql/write_idoc_typed.sql) is a template for exactly that (its final `COPY` is a
commented placeholder); `test/sql/idoc_typed_write.test` exercises its steps. Typed `DATE`/`TIME`/`DECIMAL` values must be formatted back
to SAP text first — see *Real SQL types* above.

### Convert flat ⇄ IDoc-XML

```sql
-- flat  → self-describing XML
SELECT xml FROM sap_idoc_to_xml('orders.idoc', 'orders.dict.parquet');

-- XML → flat (write it out)
COPY (SELECT raw_record FROM sap_idoc_xml_to_records('orders.xml','orders.dict.parquet') ORDER BY record_index)
  TO 'orders.idoc' (FORMAT sap_idoc);
```

Both directions take **one file path** (no glob) and load the whole file into memory, and they work on
**one IDoc per file**: `sap_idoc_to_xml` writes a second IDoc as a second root element (not well-formed XML), and
`sap_idoc_xml_to_records` reads only the first. A segment with data that the dictionary doesn't describe
makes them fail with an error naming the segment — see *Round-trip guarantees* below.

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
or inbound processing accepts. The encoders handle fixed-width packing and padding; you
compute the hierarchy fields (`SEGNUM`, `PSGNUM`, `HLEVEL`) with the template in
[`sql/write_idoc_typed.sql`](sql/write_idoc_typed.sql).

### 4. Migrate a flat-file interface to IDoc-XML (or back)
Two systems, two serializations. Convert in one step — `flat → xml → flat` is byte-exact
when the dictionary describes every segment and covers the SDATA bytes in use — so you can
switch a port’s format without touching the payload (one IDoc per file).

### 5. Typed decode on an air-gapped host
Fetch the segment dictionary **once** from a connected system, persist it to Parquet,
then decode IDocs on a detached host with **no SAP and no `erpl_rfc`**:

```sql
-- on a SAP-less machine — only erpl_idoc + the dictionary file
SELECT * FROM sap_idoc_read_segment('doc.idoc', 'E1BPSBONEW', 'flightbooking.dict.parquet');
```

### 6. Validate before you send
Check your **dictionary** for structural problems — bad offsets, overlaps, duplicate field
positions, fields past the end of `SDATA` — before you rely on it:

```sql
SELECT * FROM sap_idoc_dict_validate('mytype.dict.csv');   -- empty result = sound
```

`COPY (FORMAT sap_idoc)` additionally rejects records of the wrong width. There is no
check of an IDoc file's field values, hierarchy or business content.

---

## 📖 Function reference

### Reading

| Function | What you get |
|---|---|
| `sap_idoc_read(path [, framing, lenient, encoding])` | generic long rows: `document_key, docnum, segnum, segnam, psgnum, hlevel, mandt, sdata` |
| `sap_idoc_read_control(path [, …])` | the control record — all 36 `EDI_DC40` fields as named text columns (flat **or** XML) |
| `sap_idoc_read_segment(path, segnam, dict [, typed, strict, …])` | named columns for one segment type, sliced from `SDATA` per the dictionary (`VARCHAR`; `typed := true` for `DATE`/`TIME`/`DECIMAL`) |
| `sap_idoc_read_fields(path, dict [, include_unknown, …])` | **every field of every record** in one call — long rows: `document_key, segnum, psgnum, hlevel, segnam, field_pos, field_name, datatype, value` |
| `sap_idoc_read_raw(path [, …])` | one row per physical record with exact bytes — the byte-exact writer source |
| `sap_idoc_read_xml(path)` | generic long rows from an IDoc-XML file (self-describing; no dictionary) |

Every **flat-file** reader (`sap_idoc_read`, `_read_control`, `_read_segment`, `_read_fields`,
`_read_raw`) accepts a **single path, a glob, or a `LIST` of paths**, resolved through
DuckDB's virtual filesystem — so a whole directory works, including remote stores
(`s3://…`, `http(s)://…`, `gs://…`) once the matching extension is loaded
(`INSTALL httpfs; LOAD httpfs;`) and a `CREATE SECRET` is set for credentials:

```sql
SELECT * FROM sap_idoc_read(['a.idoc', 'b.idoc']);          -- explicit list
```


```sql
SELECT filename, idoctyp FROM sap_idoc_read_control('s3://bucket/idocs/*.idoc', filename=true);
```

The XML functions (`sap_idoc_read_xml`, `sap_idoc_to_xml`, `sap_idoc_xml_to_records`) take **one path**,
no glob or `LIST`, and read the whole file into memory — for a folder of XML files, query them one by one.

Parameters (the flat-file readers; the XML readers take only a path): `framing` = `'fixed'` (default) \| `'lf'` \| `'crlf'`
(auto-detected when omitted) · `lenient := true` salvages complete records from a
truncated file · `encoding` (see below) · `filename := true`
adds a source-file column (handy across a glob). `sap_idoc_read_fields` also takes
`include_unknown := false` to drop segments absent from the dictionary (default keeps
them as one row with the raw trimmed `SDATA`).

**Encodings.** A flat IDoc is a byte stream with fixed record widths (524 / 1063 bytes), so
`encoding` only says how the bytes of each text field become characters. Names are
case-insensitive; anything else is rejected with `unsupported encoding 'X'`.
`sap_idoc_read_raw` returns the exact bytes, so `encoding` cannot change its output (a typo is
still rejected). IDoc-XML input is already text and is never re-decoded.

> **Upgrading:** before this validation, an unknown name was silently ignored and a single-byte
> (latin-1/cp1252) file read under the default `'utf-8'` failed deep inside DuckDB or returned
> garbage. If a file was written in a single-byte code page, pass `encoding := 'latin-1'` (what
> SAP itself assumes) or `'cp1252'`.

| `encoding` | Meaning |
|---|---|
| `'utf-8'` (default; also `utf8`) | Bytes must be valid UTF-8, otherwise the query fails with a hint instead of returning garbage. Multi-byte characters make a record longer than 1063 bytes, so this suits ASCII or width-preserving files. |
| `'ascii'` (`us-ascii`) | Every byte must be below `0x80`; anything else fails the query. Use it to detect unexpected high bytes. |
| `'latin-1'` (`latin1`, `iso-8859-1`) | Byte *N* becomes U+00*N*. This is exactly what SAP does when it reads such a file (`OPEN DATASET … IN LEGACY BINARY MODE`), verified against an A4H system (`test/e2e/m8_encoding.sh`): `0x80` is the control character U+0080, not `€`. |
| `'cp1252'` (`windows-1252`) | As `latin-1`, except `0x80`–`0x9F` map to `€ ‚ ƒ „ … † ‡ ˆ ‰ Š ‹ Œ Ž ‘ ’ “ ” • – — ˜ ™ š › œ ž Ÿ`. Use it for files produced by Windows tools. SAP itself would read those bytes as latin-1. |

`sap_idoc_read_segment` additionally takes `typed := false` (default; `true` maps `DATS`/`TIMS`/`DEC`
to `DATE`/`TIME`/`DECIMAL`) and `strict := true` (default; only with `typed := true`) — see
*Real SQL types* above.

The flat-file readers are **streaming and parallel**: each file is parsed record-by-record in
constant memory (never fully buffered), and a glob/`LIST` is read with one thread per
file. Rows are therefore **unordered across files** (order within a file is preserved) —
add `ORDER BY` if you need a stable order, exactly as with `read_csv`/`read_parquet`.

**Joining across files.** `document_key` numbers the IDocs *within each file* (it restarts at 1 per file),
so over a glob join on `(filename, document_key)`, never on `document_key` alone:

```sql
SELECT c.filename, c.document_key, c.docnum, s.passname
FROM sap_idoc_read_control('inbox/*.idoc', filename=true) c
JOIN sap_idoc_read_segment('inbox/*.idoc', 'E1BPSBONEW', 'flightbooking.dict.parquet', filename=true) s
  USING (filename, document_key);
```

`sap_idoc_read_fields` also returns rows for segments missing from the dictionary (`field_name` NULL)
unless you pass `include_unknown := false` — add `WHERE field_name IS NOT NULL` when aggregating.

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
| `sap_idoc_to_xml(flat_path, dict [, encoding])` | flat → IDoc-XML text (always UTF-8; `encoding` says how the flat bytes are decoded) |
| `sap_idoc_xml_to_records(xml_path, dict)` | IDoc-XML → flat records (for `COPY … (FORMAT sap_idoc)`) |

### Dictionary tooling

| Function | Purpose |
|---|---|
| `sap_idoc_dict_offsets(src)` | compute field offsets from lengths (author a dict from field order + width) |
| `sap_idoc_dict_validate(src)` | list structural problems; empty = sound |
| `sap_idoc_dict_from_fields(fields, idoctyp, cimtyp, release)` | normalize a raw `IDOCTYPE_READ_COMPLETE` field list to the dictionary schema |
| `sap_idoc_params(idoctyp [, cimtyp, version])` | macro: the parameter struct for `sap_idoc_dictionary` (needs `erpl_rfc`) |
| `sap_idoc_dictionary(params)` | table macro: fetch + normalize a basic type's dictionary from a live system (needs `erpl_rfc`) |
| `sap_idoc_version(name)` | smoke function: returns `erpl_idoc <name>`; confirms the extension loaded |

Every function is self-documenting — `SELECT * FROM duckdb_functions() WHERE function_name LIKE 'sap_idoc_%'`
shows a description and an example for each.

---

## 🔤 The segment dictionary

Typed mode needs to know each segment’s field layout (name, offset, length, type). That
“segment dictionary” is just a **relation** with these columns:

```
idoctyp, cimtyp, release, segnam, segdef, field_pos, field_name, offset, length, datatype,
data_element, description, mandatory      -- the last three are informational; optional: decimals
```

Column names are case-insensitive. The segment lookup key is `segnam`, not `segdef`. `offset` is the
0-based position inside `SDATA`; `length` the external width in bytes.

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
  **byte-for-byte**, for any IDoc with fixed-width framing — it never interprets the payload. For an
  LF/CRLF file pass the matching `framing` to `COPY`; a last line without a terminator cannot be reproduced.
- **Flat ⇄ XML:** `flat → xml → flat` (and `xml → flat`) is byte-exact for a **single, canonically numbered
  IDoc** — sequential `SEGNUM`, hierarchy-consistent `PSGNUM`, data-record `MANDT`/`DOCNUM` equal to the
  control record's, fixed-width framing — **whose segments the dictionary fully describes and whose fields
  cover the SDATA bytes in use.** The XML form carries only the dictionary's fields, trims trailing blanks, and
  the flat side is rebuilt with recomputed `SEGNUM`/`PSGNUM`. Consequences: SDATA bytes no dictionary field
  covers, and XML elements the dictionary doesn't know for a known segment, are dropped without error; a
  whole segment missing from the dictionary raises an error (naming the document and segment) when it has
  data, and is never silently emptied. Segments with no data convert regardless.
- **System-true:** IDocs written by `erpl_idoc` — including ones converted from XML — were
  accepted by SAP inbound processing on an A4H trial system (`IDOC_INBOUND_WRITE_TO_DB`, through a small ABAP
  helper; see `test/e2e/`), not mocks. The helper first normalizes some control fields (document number,
  `MANDT`, `DIRECT`, `STATUS`) before SAP persists the IDoc. That is one SAP release and a handful of IDoc types,
  not a certification.

---

## 🔌 Live SAP, cleanly separated

`erpl_idoc` is a **pure, offline file engine** — it links no SAP libraries and makes no
network calls. When you want a live round trip, you *compose* it with
[`erpl_rfc`](https://github.com/DataZooDE/erpl) in SQL:

- **Get a dictionary** — `sap_idoc_dictionary(sap_idoc_params('ORDERS05'))` (above). This is the
  supported SQL-only live-SAP path.
- **Import a generated IDoc** — there is no one-call SQL import. On the A4H trial,
  `IDOC_INBOUND_WRITE_TO_DB` is not remote-callable, and `IDOC_INBOUND_ASYNCHRONOUS` is but runs
  asynchronously and needs partner profiles. Our end-to-end tests therefore use a thin ABAP helper class
  ([`test/e2e/abap/zcl_erpl_idoc_e2e.abap`](test/e2e/abap/zcl_erpl_idoc_e2e.abap)) that calls the inbound FM
  inside the system; another route is to place the file where a SAP inbound file port reads it.

This keeps the file format portable and dependency-free.

---

## 🧭 Scope and limits

**In scope:** flat `EDI_DC40`/`EDI_DD40`, generic + dictionary-driven read/write (text by default,
opt-in `DATE`/`TIME`/`DECIMAL`), the full control record, the segment dictionary (online/offline),
both framings, multi-IDoc files and globs, lenient/error handling, `utf-8`/`ascii`/`latin-1`/`cp1252`
decode, and IDoc-XML read/write + flat↔XML conversion.

**Not (yet):**
- `EDIDC`/`EDID4`/`EDIDS` **table** input or status-record side output — IDoc content read from SAP tables must be
  re-encoded by hand;
- X12/EDIFACT conversion;
- business-semantic validation (only dictionary and record-width structure is checked);
- glob/`LIST`, streaming, parallelism and multi-IDoc files for the **XML** functions (one IDoc per file, in memory);
- dictionary → `CREATE TABLE` DDL export;
- a one-call SQL import into SAP (see *Live SAP, cleanly separated* below).

**Good to know:**
- Fixed record widths are in **bytes** (524 control / 1063 data), so multi-byte UTF-8 characters can overflow a
  record; use `latin-1` or ASCII data for flat files.
- `document_key` restarts per file — join on `(filename, document_key)`.
- `COPY` writes rows in the order given; always `ORDER BY record_index` (or your own ordering column).
- `sap_idoc_read_segment` column names are lower-case; `sap_idoc_read_fields` and `sap_idoc_read_xml` return
  `field_name` exactly as the dictionary / XML tag spells it (SAP's upper-case names for SAP-sourced dictionaries).

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

## 💬 Feedback

If `erpl_idoc` misreads a file, writes something SAP rejects, or does anything
surprising, please [open an issue](https://github.com/DataZooDE/erpl-idoc/issues).
IDoc layouts vary by release, segment version and customer extension in ways we cannot
reproduce here, so a report with a (redacted) sample is the fastest path to a fix.
Every error the extension raises ends with that link.

If it saved you time, a star on the repo helps other people find it.

The first time you load the extension in an interactive terminal each day, a small
banner says the same thing. It never prints when output is piped, in notebooks, or in
CI. Silence it with `SET datazoo_banner = false;` or `DATAZOO_NO_BANNER=1`.

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
