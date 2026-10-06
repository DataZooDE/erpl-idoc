-- =====================================================================
-- Build a flat IDoc file from an IDoc stored in SAP (EDIDC + EDID4)
-- =====================================================================
-- A documented SQL recipe (NOT C++): it reads one IDoc's control record from
-- EDIDC and its data records from EDID4 through the sibling erpl_rfc extension,
-- then encodes them with erpl_idoc's pure encoders and writes the file with
-- COPY (FORMAT sap_idoc). erpl_idoc itself performs no RFC.
--
-- Verified against an A4H trial (test/e2e/m11_edidc_edid4.sh).
--
-- Prerequisites:
--   LOAD erpl_rfc;  LOAD erpl_idoc;
--   CREATE SECRET a4h (TYPE sap_rfc, ASHOST '...', SYSNR '00', CLIENT '001',
--                      USER '...', PASSWD '...', LANG 'EN');
--
-- Replace the two placeholders below: <<<DOCNUM>>> (16 digits, e.g. 0000000000000013)
-- and <<<OUT>>> (target path). They must be LITERALS: sap_read_table and
-- sap_rfc_invoke evaluate their arguments at BIND time, so a table macro or a
-- parameter cannot carry them.
--
-- WHY EDID4 IS NOT READ WITH sap_read_table
--   EDID4-SDATA is a DDIC LCHR field (1000 characters plus a length field,
--   DTINT2). RFC_READ_TABLE cannot return it: any select list containing SDATA
--   fails with "Only the prefixed literals are allowed" (the metadata columns
--   alone read fine). The Data Services variant /SAPDS/RFC_READ_TABLE2 can; it
--   ships with the ST-PI add-on, so check that it exists on your system
--   (sap_rfc_describe_function('/SAPDS/RFC_READ_TABLE2')). Not every system has it.
--   Without a delimiter the fields come back as one fixed-width string
--   (SEGNUM 6, SEGNAM 30, PSGNUM 6, HLEVEL 2, SDATA the rest, trailing blanks
--   trimmed), so a '|' inside SDATA cannot break parsing. A segment row is at
--   most 1044 bytes, so it always lands in TBLOUT2048.
--
-- FILTER FORM
--   Observed on the A4H trial: FILTER='DOCNUM EQ ''..''' works for both EDIDC and EDID4
--   (and is what this recipe uses). A plain FILTER='DOCNUM = ''..''' was rejected for
--   EDID4 (it worked with a trailing blank), and a DuckDB WHERE on sap_read_table is
--   not pushed down in a form EDID4 accepts. Keep filters as literals in FILTER/OPTIONS.
-- =====================================================================

-- 1) Control record: EDIDC has one row per IDoc.
CREATE OR REPLACE TEMP TABLE edidc_src AS
    SELECT * FROM sap_read_table('EDIDC', FILTER='DOCNUM EQ ''<<<DOCNUM>>>''');

-- 2) Data records: one row per segment, fixed-width, in key order.
CREATE OR REPLACE TEMP TABLE edid4_src AS
    SELECT substr(u.WA, 1, 6)                AS segnum,
           trim(substr(u.WA, 7, 30))         AS segnam,
           substr(u.WA, 37, 6)               AS psgnum,
           substr(u.WA, 43, 2)               AS hlevel,
           substr(u.WA, 45)                  AS sdata      -- trailing blanks trimmed by SAP; re-padded below
    FROM sap_rfc_invoke('/SAPDS/RFC_READ_TABLE2',
             {'QUERY_TABLE': 'EDID4',
              'FIELDS'     : [{'FIELDNAME': 'SEGNUM'}, {'FIELDNAME': 'SEGNAM'}, {'FIELDNAME': 'PSGNUM'},
                              {'FIELDNAME': 'HLEVEL'}, {'FIELDNAME': 'SDATA'}],
              'OPTIONS'    : [{'TEXT': 'DOCNUM EQ ''<<<DOCNUM>>>'''}]}) r,
         UNNEST(r.TBLOUT2048) AS t(u);

-- 2b) Fail loudly instead of writing a wrong file.
SELECT error('IDoc <<<DOCNUM>>> not found in EDIDC') WHERE (SELECT count(*) FROM edidc_src) <> 1;
SELECT error('IDoc <<<DOCNUM>>> has no EDID4 segments') WHERE (SELECT count(*) FROM edid4_src) = 0;
SELECT error('duplicate SEGNUM in EDID4 for IDoc <<<DOCNUM>>>')
WHERE (SELECT count(*) - count(DISTINCT segnum) FROM edid4_src) > 0;

-- 3) Encode and write. The control record comes first, then the segments in SEGNUM order.
--    Column mapping EDIDC -> EDI_DC40 (36 fields, in file order):
--      TABNAM  = 'EDI_DC40' (not in EDIDC)          IDOCTYP = EDIDC.IDOCTP (renamed)
--      CREDAT / CRETIM: erpl_rfc returned DATE / TIME on the trial; the file wants YYYYMMDD / HHMMSS.
--      Casting to text and keeping the digits works for DATE/TIME and for text columns alike
--      (an SAP '00000000' date stays '00000000'; NULL becomes blank).
--      every other field has the same name in EDIDC.
--    Placeholders: <<<DOCNUM>>> must be 16 digits and <<<OUT>>> must not contain a single
--    quote — they are pasted into SQL string literals (and, for DOCNUM, into the ABAP WHERE).
--    STATUS and DIRECT are copied as stored (the IDoc's DATABASE status and direction).
--    Override them here if the consumer of the file expects something else, e.g.
--    STATUS '30' (ready for dispatch) and DIRECT '1' (outbound) for an outbound file port.
COPY (
    SELECT raw FROM (
        SELECT 0 AS ord,
               sap_idoc_encode_control([
                   'EDI_DC40', c.MANDT, c.DOCNUM, c.DOCREL, c.STATUS, c.DIRECT, c.OUTMOD, c.EXPRSS, c.TEST,
                   c.IDOCTP, c.CIMTYP, c.MESTYP, c.MESCOD, c.MESFCT, c.STD, c.STDVRS, c.STDMES,
                   c.SNDPOR, c.SNDPRT, c.SNDPFC, c.SNDPRN, c.SNDSAD, c.SNDLAD,
                   c.RCVPOR, c.RCVPRT, c.RCVPFC, c.RCVPRN, c.RCVSAD, c.RCVLAD,
                   left(COALESCE(regexp_replace(CAST(c.CREDAT AS VARCHAR), '[^0-9]', '', 'g'), ''), 8),
                   left(COALESCE(regexp_replace(CAST(c.CRETIM AS VARCHAR), '[^0-9]', '', 'g'), ''), 6),
                   c.REFINT, c.REFGRP, c.REFMES, c.ARCKEY, c.SERIAL]) AS raw
        FROM edidc_src c
        UNION ALL
        SELECT CAST(s.segnum AS INTEGER),
               sap_idoc_encode_data_record(s.segnam, c.MANDT, CAST(c.DOCNUM AS BIGINT), CAST(s.segnum AS BIGINT),
                                           CAST(s.psgnum AS BIGINT), CAST(s.hlevel AS INTEGER),
                                           sap_idoc_encode_sdata([0], [1000], [s.sdata]))
        FROM edid4_src s, edidc_src c
    ) ORDER BY ord
) TO '<<<OUT>>>' (FORMAT sap_idoc);
