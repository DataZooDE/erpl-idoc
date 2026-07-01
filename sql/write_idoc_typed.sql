-- =====================================================================
-- Typed IDoc write recipe (FR-W2 / FR-W3) — compose a byte-valid IDoc file
-- from typed segment values + a segment dictionary, recomputing derived fields.
-- =====================================================================
-- Pattern (no C++ beyond the pure idoc_encode_* scalars; no RFC):
--   1. SEGMENT INPUT: the segments to emit, in order, with their HLEVEL. This is
--      typed input — the caller decides structure; SEGNUM/PSGNUM are NOT input.
--   2. TYPED FIELD VALUES: (segnam, field_name, value) triples.
--   3. DICTIONARY: offsets/lengths per (segnam, field_name) (SPEC B4). Offline CSV,
--      online recipe (sql/sap_idoc_dictionary.sql), or a hand-authored table.
--   4. RECOMPUTE derived fields: SEGNUM = sequential; PSGNUM = SEGNUM of the nearest
--      preceding segment at HLEVEL-1; HLEVEL from input.
--   5. ENCODE: idoc_encode_sdata -> idoc_encode_data_record; idoc_encode_control.
--   6. WRITE: COPY (raw records, control first) TO '...' (FORMAT idoc).
--
-- The blocks below are parameterized by three TEMP tables the caller populates:
--   idoc_seg_input(ord INT, segnam VARCHAR, hlevel INT)
--   idoc_field_values(segnam VARCHAR, field_name VARCHAR, value VARCHAR)
--   idoc_control_values(field_name VARCHAR, value VARCHAR)   -- 36 EDI_DC40 fields
-- plus a `dict` relation (the B4 dictionary) and a target path.
-- =====================================================================

-- Recompute SEGNUM (sequential) then PSGNUM (parent = nearest prior HLEVEL-1).
CREATE OR REPLACE TEMP TABLE idoc_seg_num AS
    SELECT ord, segnam, hlevel, row_number() OVER (ORDER BY ord) AS segnum
    FROM idoc_seg_input;

-- PSGNUM = SEGNUM of the nearest preceding segment at HLEVEL-1 = MAX(segnum) among
-- prior rows one level up (self-join avoids a correlated subquery).
CREATE OR REPLACE TEMP TABLE idoc_seg_hier AS
    SELECT s.ord, s.segnam, s.hlevel, s.segnum,
           COALESCE(MAX(p.segnum), 0) AS psgnum
    FROM idoc_seg_num s
    LEFT JOIN idoc_seg_num p ON p.hlevel = s.hlevel - 1 AND p.segnum < s.segnum
    GROUP BY s.ord, s.segnam, s.hlevel, s.segnum;

-- Compose SDATA per segment from typed values + dictionary slicing.
CREATE OR REPLACE TEMP TABLE idoc_sdata AS
    SELECT h.ord, h.segnam,
           COALESCE(
               idoc_encode_sdata(list(d."offset" ORDER BY d.field_pos),
                                 list(d.length   ORDER BY d.field_pos),
                                 list(COALESCE(v.value, '') ORDER BY d.field_pos)),
               repeat(' ', 1000)) AS sdata
    FROM idoc_seg_hier h
    LEFT JOIN dict d ON d.segnam = h.segnam
    LEFT JOIN idoc_field_values v ON v.segnam = h.segnam AND v.field_name = d.field_name
    GROUP BY h.ord, h.segnam;

-- Assemble the file: control record first (ord 0), then data records in order.
-- (docnum is a policy input; here 0 = blank/renumber-on-import.)
-- COPY (
--     SELECT raw FROM (
--         SELECT 0 AS ord, idoc_encode_control(
--             list(cv.value ORDER BY cv.field_pos)) AS raw
--         FROM (SELECT iv.field_name, iv.value,
--                      list_position(['TABNAM','MANDT','DOCNUM','DOCREL','STATUS','DIRECT','OUTMOD',
--                        'EXPRSS','TEST','IDOCTYP','CIMTYP','MESTYP','MESCOD','MESFCT','STD','STDVRS',
--                        'STDMES','SNDPOR','SNDPRT','SNDPFC','SNDPRN','SNDSAD','SNDLAD','RCVPOR','RCVPRT',
--                        'RCVPFC','RCVPRN','RCVSAD','RCVLAD','CREDAT','CRETIM','REFINT','REFGRP','REFMES',
--                        'ARCKEY','SERIAL'], iv.field_name) AS field_pos
--               FROM idoc_control_values iv) cv
--         UNION ALL
--         SELECT h.ord,
--                idoc_encode_data_record(h.segnam, '001', <docnum>, h.segnum, h.psgnum, h.hlevel, sd.sdata)
--         FROM idoc_seg_hier h JOIN idoc_sdata sd USING (ord, segnam)
--     ) ORDER BY ord
-- ) TO '<path>' (FORMAT idoc);
