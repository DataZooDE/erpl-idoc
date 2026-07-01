-- =====================================================================
-- dict_helpers.sql — build & validate an OFFLINE IDoc segment dictionary
-- without any SAP connection (hand-authoring / air-gapped path).
-- =====================================================================
-- The dictionary the typed reader consumes needs, per field:
--   segnam, field_pos, field_name, offset, length, datatype
-- (the full SPEC B4 schema adds idoctyp, cimtyp, release, segdef,
--  data_element, description, mandatory — optional for reading).
--
-- These are plain SQL patterns (no C++, no RFC). Substitute your own input.
-- =====================================================================

-- ---------------------------------------------------------------------
-- 1) OFFSET-FROM-LENGTHS
-- Supply only the field ORDER and WIDTH; the 0-based SDATA offset of each
-- field is the cumulative width of the preceding fields in the same segment.
-- ---------------------------------------------------------------------
-- Example input (edit these VALUES):
WITH dict_in(segnam, field_pos, field_name, length, datatype) AS (
    VALUES
      ('E1BPSBONEW', 1, 'AIRLINEID',  3, 'CHAR'),
      ('E1BPSBONEW', 2, 'CONNECTID',  4, 'NUMC'),
      ('E1BPSBONEW', 3, 'FLIGHTDATE', 8, 'DATS'),
      ('E1BPSBONEW', 4, 'CUSTOMERID', 8, 'NUMC'),
      ('E1BPSBONEW', 5, 'CLASS',      1, 'CHAR')
)
SELECT
    segnam, field_pos, field_name,
    COALESCE(SUM(length) OVER (PARTITION BY segnam ORDER BY field_pos
                               ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING), 0) AS "offset",
    length, datatype
FROM dict_in
ORDER BY segnam, field_pos;
-- Persist it:  COPY ( <the query above> ) TO 'my.dict.csv' (FORMAT csv, HEADER true);

-- ---------------------------------------------------------------------
-- 2) VALIDATE a dictionary before using it. Replace <DICT> with your source
-- (a table/view name, or read_csv_auto('my.dict.csv')). Returns one row per
-- PROBLEM found — an empty result means the dictionary is structurally sound.
-- ---------------------------------------------------------------------
WITH d AS (SELECT * FROM read_csv_auto('my.dict.csv')),   -- <DICT>
     ordered AS (
       SELECT *, LAG(CAST("offset" AS BIGINT) + CAST(length AS BIGINT))
                   OVER (PARTITION BY segnam ORDER BY field_pos) AS prev_end
       FROM d
     )
SELECT segnam, field_name, "offset", length, 'offset<0 or length<=0'  AS problem
FROM d WHERE CAST("offset" AS BIGINT) < 0 OR CAST(length AS BIGINT) <= 0
UNION ALL
SELECT segnam, field_name, "offset", length, 'field exceeds SDATA(1000)'
FROM d WHERE CAST("offset" AS BIGINT) + CAST(length AS BIGINT) > 1000
UNION ALL
SELECT segnam, field_name, "offset", length, 'overlaps the previous field'
FROM ordered WHERE prev_end IS NOT NULL AND CAST("offset" AS BIGINT) < prev_end
UNION ALL
SELECT segnam, CAST(field_pos AS VARCHAR), NULL, NULL, 'duplicate field_pos in segment'
FROM d GROUP BY segnam, field_pos HAVING count(*) > 1
ORDER BY segnam, problem;

-- ---------------------------------------------------------------------
-- 3) FROM DDIC (alternative online source; needs erpl_rfc) — when
-- IDOCTYPE_READ_COMPLETE is restricted, read the definition tables directly:
--   SELECT * FROM sap_read_table('EDISDEF', SECRET='s');   -- segment defs + lengths
--   SELECT * FROM sap_read_table('EDSAPPL', SECRET='s');   -- per-field name/length
-- then map EDSAPPL's (SEGMENT, FIELDNAME, EXTLEN, cumulative BYTE) to the B4 columns
-- exactly as in pattern (1).
-- =====================================================================
