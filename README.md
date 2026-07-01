# erpl_idoc — SAP IDoc flat files for DuckDB

`erpl_idoc` makes **DuckDB a first-class SAP IDoc reader and writer**: query IDoc
flat files as SQL tables, and emit byte-valid IDoc files from SQL. It extends the
[erpl](https://github.com/DataZooDE/erpl) SAP data-plane family (`erpl_rfc`,
`erpl_odp`, `erpl_bics`) down to the document/EDI layer, and **composes with
`erpl_rfc`** for optional live-SAP round trips.

- **Flat-file core is pure and offline** — no SAP, no NW RFC SDK. Builds and works
  on Linux/macOS/Windows/Wasm with `erpl_rfc` absent.
- **Generic mode** (raw records, byte-exact round trip) and **typed mode**
  (`SDATA` split into typed columns via a segment dictionary).
- **Dictionary as data** — the same DuckDB relation whether it came from a live
  system (via `erpl_rfc`), a persisted Parquet/CSV, or a hand-authored table.
- **No RFC inside `erpl_idoc`** — every live-SAP interaction is `erpl_rfc`,
  orchestrated in SQL.

## SQL surface

### Reader

| Function | Result |
|---|---|
| `sap_idoc_read(path [, framing, lenient, encoding])` | generic long: one row per **data** record — `document_key, docnum, segnum, segnam, psgnum, hlevel, mandt, sdata` |
| `sap_idoc_read_control(path [, …])` | typed control record — all 36 `EDI_DC40` fields |
| `sap_idoc_read_raw(path [, …])` | one row per **physical** record with exact bytes — `document_key, record_index, record_type ('C'/'D'), raw_record BLOB` |
| `sap_idoc_read_segment(path, segnam, dict [, framing, lenient, encoding])` | typed: one column per dictionary field of `segnam`, sliced from `SDATA` |

Parameters: `framing` = `'fixed'` (default, contiguous) \| `'lf'` \| `'crlf'`
(auto-detected when omitted); `lenient` = salvage complete records from a truncated
file; `encoding` = `'utf-8'` (default) \| `'latin-1'` (SDATA text decode).

### Writer

```sql
COPY (<single BLOB/VARCHAR column of raw records>) TO 'file.idoc' (FORMAT sap_idoc [, framing 'fixed'|'lf'|'crlf']);
```

The writer frames raw records into a file. Compose records with the pure encoders:

| Scalar | Result |
|---|---|
| `sap_idoc_encode_sdata(offsets LIST<INT>, lengths LIST<INT>, values LIST<VARCHAR>)` | a 1000-byte `SDATA` |
| `sap_idoc_encode_data_record(segnam, mandt, docnum, segnum, psgnum, hlevel, sdata)` | a 1063-byte `EDI_DD40` BLOB |
| `sap_idoc_encode_control(values LIST<VARCHAR>)` | a 524-byte `EDI_DC40` BLOB |

### Round-trip contract (holds byte-for-byte)

```sql
COPY (SELECT raw_record FROM sap_idoc_read_raw('f.idoc') ORDER BY record_index)
  TO 'g.idoc' (FORMAT sap_idoc);        -- g.idoc ≡ f.idoc, byte-for-byte
```

## Typed mode & the segment dictionary

The dictionary is a relation with the schema (SPEC B4):
`idoctyp, cimtyp, release, segnam, segdef, field_pos, field_name, offset, length,
datatype, data_element, description, mandatory`. Its **origin is irrelevant** to the
parser:

- **Offline** — a CSV/Parquet file or any table/view:
  ```sql
  SELECT airlineid, flightdate
  FROM sap_idoc_read_segment('flight.idoc', 'E1BPSBONEW', 'flight_dict.csv');
  ```
- **Online (composes `erpl_rfc`)** — the extension ships two SQL macros,
  `sap_idoc_params(idoctyp [,cimtyp,version])` and `sap_idoc_dictionary(params)`:
  ```sql
  LOAD erpl_rfc; LOAD erpl_idoc;
  CREATE SECRET a4h (TYPE sap_rfc, ASHOST '…', SYSNR '00', CLIENT '001', USER '…', PASSWD '…', LANG 'EN');
  SELECT * FROM sap_idoc_dictionary(sap_idoc_params('FLIGHTBOOKING_CREATEFROMDAT01'));
  ```
  Persist it once to seed repeatable offline runs (the **connected → detached-host** bridge):
  ```sql
  COPY (SELECT * FROM sap_idoc_dictionary(sap_idoc_params('FLIGHTBOOKING_CREATEFROMDAT01')))
       TO 'flight.dict.parquet' (FORMAT parquet);
  ```
  Then on a SAP-less host, `sap_idoc_read_segment(idoc, seg, 'flight.dict.parquet')` — no `erpl_rfc`.
  (`erpl_idoc` never calls RFC; the macro composes `erpl_rfc` at the SQL layer. The two-macro
  split is required because `sap_rfc_invoke` evaluates its arg at bind time — see
  [`sql/sap_idoc_dictionary.sql`](sql/sap_idoc_dictionary.sql).)

### Creating dictionary files

- **In SQL** (online → file), using the shipped macros + native `COPY`:
  ```sql
  COPY (SELECT * FROM sap_idoc_dictionary(sap_idoc_params('MATMAS05'))) TO 'matmas05.dict.parquet' (FORMAT parquet);
  ```
- **Export helper** (turn-key, batch): [`scripts/export_idoc_dict.sh`](scripts/export_idoc_dict.sh)
  ```sh
  scripts/export_idoc_dict.sh FLIGHTBOOKING_CREATEFROMDAT01 --format parquet --out flight.dict.parquet
  scripts/export_idoc_dict.sh --batch types.txt --format parquet --out-dir dicts/   # many types
  ```
- **Hand-authoring** (offline, no SAP) — extension functions:
  - `sap_idoc_dict_offsets(src)` — supply only `segnam, field_pos, field_name, length[, datatype]`;
    offsets are computed from cumulative widths.
  - `sap_idoc_dict_validate(src)` — returns one row per structural problem (out-of-bounds
    offsets, overlaps, non-positive lengths, duplicate positions); empty = sound.
  ```sql
  COPY (SELECT * FROM sap_idoc_dict_offsets('my_fields.csv')) TO 'my.dict.parquet' (FORMAT parquet);
  SELECT * FROM sap_idoc_dict_validate('my.dict.parquet');   -- should return no rows
  ```
- **Normalizer** `sap_idoc_dict_from_fields(PT_FIELDS, idoctyp, cimtyp, release)` — maps a raw
  `IDOCTYPE_READ_COMPLETE` field list to the B4 schema (what `sap_idoc_dictionary` uses
  internally; call it directly if you issue the `sap_rfc_invoke` yourself).
- **From DDIC**: `sap_read_table('EDISDEF')` + `sap_read_table('EDSAPPL')` when
  `IDOCTYPE_READ_COMPLETE` is restricted (see `sql/dict_helpers.sql`).
  (`src` is a `.csv`/`.parquet` path, a table/view name, or a relation expression.)

## Typed write

See [`sql/write_idoc_typed.sql`](sql/write_idoc_typed.sql): compose `SDATA` from typed
values + the dictionary, **recompute derived fields** (`SEGNUM` sequential, `PSGNUM`
= parent at `HLEVEL-1`, `HLEVEL` from input), encode records, and `COPY (… FORMAT sap_idoc)`.

## Live-SAP composition (optional)

`erpl_idoc` never calls RFC. Live exchange is documented SQL that composes `erpl_rfc`:
fetch a dictionary (above), or import an IDoc the extension wrote. Note the
`IDOCTP`(DB table `EDIDC`) vs `IDOCTYP`(flat `EDI_DC40`) field-name divergence — the
reader/writer map it explicitly.

## Build & test

```sh
make debug         # build extension (DuckDB v1.5.4 submodule) + test runner
make test          # DuckDB SQL tests (test/sql/*.test)
make sql_tests     # same, one file at a time
make core_tests    # fast, DuckDB-free unit tests for the format core (Catch2)
make e2e           # live A4H end-to-end (needs erpl_rfc + a reachable SAP system; self-skips otherwise)
```

The offline SQL suite and `core_tests` pass **with `erpl_rfc` absent** — that is the
cross-platform CI gate. Live E2E tests (dictionary symmetry, typed read, and A4H
inbound acceptance) run against the local `sapse/abap-cloud-developer-trial` (A4H).

## Distribution

Community-extension descriptor: [`packaging/description.yml`](packaging/description.yml)
(min DuckDB **v1.5.4**, built against the stable C-extension API). Install once
published:

```sql
INSTALL erpl_idoc FROM community;
LOAD erpl_idoc;
```

## Status / scope (v1)

In scope: flat-file `EDI_DC40`/`EDI_DD40`, generic + typed read/write, control-record
parse/emit, dictionary (online/offline), both framings, multi-IDoc, lenient/error
handling, `latin-1`/UTF-8 SDATA decode. Not yet: IDoc-XML, `EDIDS` **status records**
side output (FR-R7 — deferred, no fixture available), X12/EDIFACT conversion,
business-semantic validation.
