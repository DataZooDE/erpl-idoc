-- =====================================================================
-- sap_idoc_dictionary — ONLINE segment-dictionary recipe (composes erpl_rfc)
-- =====================================================================
-- A documented SQL recipe (NOT C++, and NOT a MACRO). It materializes the IDoc
-- segment dictionary for a basic type from a live SAP system by calling the
-- RFC-enabled function module IDOCTYPE_READ_COMPLETE via erpl_rfc's
-- sap_rfc_invoke. The resulting relation matches the offline dictionary schema
-- (SPEC B4), so the typed reader/writer behave identically whether the dictionary
-- came from a live system, a persisted CSV/Parquet, or a hand-authored table
-- (SPEC FR-D1/FR-D3). erpl_idoc itself performs no RFC (FR-C2).
--
-- NOTE ON MACROS: a DuckDB table MACRO cannot wrap this because sap_rfc_invoke
-- evaluates its arguments at BIND time (it connects to SAP to discover the result
-- schema), before macro parameters are substituted. Use this template directly,
-- substituting the two literals marked <<< ... >>> below, or wrap it from the host
-- application. To turn a one-time fetch into a reusable offline dictionary, persist
-- the result (see the COPY line) and thereafter read the file (FR-D5).
--
-- Prerequisites (run once in the session):
--   LOAD erpl_rfc;  LOAD erpl_idoc;
--   CREATE SECRET a4h (TYPE sap_rfc, ASHOST '...', SYSNR '00', CLIENT '001',
--                      USER '...', PASSWD '...', LANG 'EN');
-- =====================================================================

WITH raw AS (
    SELECT PE_HEADER, PT_FIELDS, PT_SEGMENTS
    FROM sap_rfc_invoke('IDOCTYPE_READ_COMPLETE',
                        {'PI_IDOCTYP': '<<<IDOCTYP>>>',   -- e.g. FLIGHTBOOKING_CREATEFROMDAT01
                         'PI_CIMTYP' : '<<<CIMTYP>>>',    -- extension, or '' if none
                         'PI_VERSION': '4'})
),
segs AS (
    SELECT s.SEGMENTTYP AS seg, s.SEGMENTDEF AS segdef, s.HLEVEL AS hlevel
    FROM raw, UNNEST(PT_SEGMENTS) AS t(s)
)
SELECT
    '<<<IDOCTYP>>>'                                  AS idoctyp,
    '<<<CIMTYP>>>'                                   AS cimtyp,
    (SELECT PE_HEADER.RELEASED FROM raw)             AS release,
    f.SEGMENTTYP                                     AS segnam,
    (SELECT segdef FROM segs WHERE seg = f.SEGMENTTYP LIMIT 1) AS segdef,
    CAST(f.FIELD_POS AS INTEGER)                     AS field_pos,
    f.FIELDNAME                                      AS field_name,
    CAST(f.BYTE_FIRST AS INTEGER)                    AS "offset",   -- 0-based offset in SDATA
    CAST(f.EXTLEN AS INTEGER)                        AS length,     -- external (flat) length
    f.DATATYPE                                       AS datatype,   -- CHAR / NUMC / DATS / ...
    f.ROLLNAME                                       AS data_element,
    f.DESCRP                                         AS description,
    false                                            AS mandatory
FROM raw, UNNEST(PT_FIELDS) AS t(f)
ORDER BY segnam, field_pos;

-- Persist for offline/CI reuse (FR-D5):
--   COPY ( <the SELECT above> ) TO 'flight_dict.csv' (FORMAT csv, HEADER true);
