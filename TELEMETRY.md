# erpl_idoc Telemetry

`erpl_idoc` collects **anonymous, privacy-preserving usage telemetry** so we can
see which capabilities are used, on which platforms, and where they fail — and
prioritise accordingly. It is **on by default** and **trivial to turn off**.

Telemetry is emitted through the shared
[`DataZooDE/posthog-telemetry`](https://github.com/DataZooDE/posthog-telemetry)
library and follows the cross-product **`telemetry_schema: 2`** envelope.
Ingestion is the EU PostHog cloud.

## How to turn it off

Any one of these fully short-circuits telemetry — when disabled, **nothing
leaves the machine** (the opt-out is enforced at the transport, not just at the
call sites):

```sql
SET erpl_telemetry_enabled = false;   -- DuckDB setting (per session)
```

```bash
export DATAZOO_DISABLE_TELEMETRY=1     # environment (1|true|yes)
```

## The guarantee: bounded, enumerated, non-PII

Every property we send is **either** a constant drawn from a small,
code-controlled enumeration **or** a pure number (durations, counts). The
library additionally clamps every outgoing string to 512 bytes as a backstop.

We **never** send: IDoc file paths or names, IDoc contents, control-record or
segment field values, message/basic types, dictionary contents, segment or
field names, SQL text, row/result data, or any error messages. The only
free-form identifier we emit is the DuckDB **function name** of an `erpl_idoc`
table function (a fixed, code-defined string such as `sap_idoc_read`), never any
argument passed to it.

The instrumentation is a single call at each function's **bind** site (not on
any per-row scan path), so a large multi-file read produces O(1) telemetry, not
a firehose.

## What is collected

### Envelope (attached to every event)

`product` (`erpl_idoc`), `product_version`, `product_edition` (`oss`),
`telemetry_schema` (`2`), `duckdb_version`, `os`, `arch`, `platform`, `is_ci`,
`is_container`, a per-process `$session_id`, and — once associated — the
`deployment` group. `distinct_id` is the SHA-256 of a machine id: a **stable,
pseudonymous** identifier, not tied to any personal data.

### Events

| Event | When | Properties (beyond the envelope) |
|---|---|---|
| `extension_loaded` | the `erpl_idoc` extension loads | — |
| `function_executed` | an `erpl_idoc` table function is bound — **aggregated** per function per session (not per row) | `function_name`, `call_count`, `duration_ms_p50` |

This repository emits **no** `feature_used` and **no** `$exception` events — it
associates only the `deployment` group and records the two events above.
`erpl_idoc` is offline core; live-SAP feature/error instrumentation lives in
`erpl_rfc`.

### Instrumented functions (`function_name` values)

`sap_idoc_read`, `sap_idoc_read_control`, `sap_idoc_read_raw`,
`sap_idoc_read_segment`, `sap_idoc_read_fields`, `sap_idoc_read_xml`,
`sap_idoc_to_xml`, `sap_idoc_xml_to_records`, `sap_idoc_dict_offsets`,
`sap_idoc_dict_validate`.

## Enterprise / account analytics

OSS `erpl_idoc` associates only the `deployment` group. Enterprise builds with a
license key would additionally associate an `account` group keyed on
`sha256(license_id)` (the raw key is hashed, never sent). OSS `erpl_idoc` has no
license key, so this is not active here.

## Function-call aggregation

DuckDB function calls are recorded via `RecordFunctionCall(function_name)`, which
aggregates in-process into a single `function_executed` event per function per
session (carrying `call_count` and `duration_ms_p50`). This is never done on a
per-row path, so a million-row read produces O(1) telemetry rows, not a
firehose.
