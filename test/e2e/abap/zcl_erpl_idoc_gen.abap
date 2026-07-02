CLASS zcl_erpl_idoc_gen DEFINITION PUBLIC FINAL CREATE PUBLIC.
  " Mass-produce byte-exact flat IDoc files for standard basic types, straight from
  " this SAP system's own DDIC segment definitions — the same EDI_DC40/EDI_DD40
  " record structures a WE21 file port writes. For each requested IDoc type we read
  " the complete segment structure (IDOCTYPE_READ_COMPLETE) and emit one control
  " record + one data record per segment (SEGNUM/PSGNUM/HLEVEL computed from the
  " hierarchy), serialized with OPEN DATASET ... LEGACY BINARY MODE.
  "
  " SDATA is filled *structurally* (the segment name), not with business data: this
  " corpus is for exercising a flat-file parser/writer (framing, offsets, widths,
  " hierarchy, multi-segment, volume), so it carries no real/PII payload.
  "
  " Type list: /tmp/idoc_corpus/types.txt (one "IDOCTYP [VERSION]" per line, '#'
  " comments allowed). If absent/empty a built-in list of common standard types is
  " used. Output: /tmp/idoc_corpus/<IDOCTYP>.idoc. Missing types are skipped, not fatal.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    TYPES: BEGIN OF ty_spec,
             idoctyp TYPE edi_dc40-idoctyp,
             version TYPE c LENGTH 1,
           END OF ty_spec,
           tt_spec TYPE STANDARD TABLE OF ty_spec WITH EMPTY KEY.
    DATA out TYPE REF TO if_oo_adt_classrun_out.
    METHODS default_types  RETURNING VALUE(rt) TYPE tt_spec.
    METHODS read_type_list IMPORTING path TYPE string RETURNING VALUE(rt) TYPE tt_spec.
    METHODS mestyp_of      IMPORTING idoctyp TYPE edi_dc40-idoctyp
                           RETURNING VALUE(rv) TYPE edi_dc40-mestyp.
    METHODS gen_one        IMPORTING spec TYPE ty_spec dir TYPE string index TYPE i.
ENDCLASS.

CLASS zcl_erpl_idoc_gen IMPLEMENTATION.

  METHOD if_oo_adt_classrun~main.
    me->out = out.
    DATA(dir) = `/tmp/idoc_corpus`.

    DATA(specs) = read_type_list( |{ dir }/types.txt| ).
    IF specs IS INITIAL.
      specs = default_types( ).
      out->write( |NOTE source=built-in-defaults count={ lines( specs ) }| ).
    ELSE.
      out->write( |NOTE source=types.txt count={ lines( specs ) }| ).
    ENDIF.

    " Synthetic, monotonically increasing DOCNUM base (a file port keeps the original
    " document number; any 16-digit value is fine for a parser corpus).
    DATA lv_idx TYPE i VALUE 1000000000.
    LOOP AT specs INTO DATA(sp).
      lv_idx = lv_idx + 1.
      gen_one( spec = sp dir = dir index = lv_idx ).
    ENDLOOP.
    out->write( |DONE types={ lines( specs ) }| ).
  ENDMETHOD.

  METHOD gen_one.
    DATA: lt_segments TYPE STANDARD TABLE OF edi_iapi11,
          lt_fields   TYPE STANDARD TABLE OF edi_iapi12,
          ls_header   TYPE edi_iapi10.

    CALL FUNCTION 'IDOCTYPE_READ_COMPLETE'
      EXPORTING  pi_idoctyp  = spec-idoctyp
                 pi_version  = spec-version
      IMPORTING  pe_header   = ls_header
      TABLES     pt_segments = lt_segments
                 pt_fields   = lt_fields
      EXCEPTIONS OTHERS      = 1.
    IF sy-subrc <> 0 OR lt_segments IS INITIAL.
      out->write( |SKIP type={ spec-idoctyp } reason=read_complete_subrc={ sy-subrc }_segs={ lines( lt_segments ) }| ).
      RETURN.
    ENDIF.

    DATA(path) = |{ dir }/{ spec-idoctyp }.idoc|.

    " --- control record (EDI_DC40, 524 bytes) -------------------------------
    DATA lc_dc40 TYPE edi_dc40.
    CLEAR lc_dc40.
    lc_dc40-tabnam  = 'EDI_DC40'.
    lc_dc40-mandt   = sy-mandt.
    lc_dc40-docnum  = index.
    lc_dc40-docrel  = sy-saprl.
    lc_dc40-status  = '30'.
    lc_dc40-direct  = '1'.
    lc_dc40-outmod  = '2'.
    lc_dc40-idoctyp = spec-idoctyp.                 " flat record field is IDOCTYP
    lc_dc40-mestyp  = mestyp_of( spec-idoctyp ).
    lc_dc40-sndpor  = 'SAPA4H'.
    lc_dc40-sndprt  = 'LS'.
    lc_dc40-sndprn  = 'A4HCLNT001'.
    lc_dc40-rcvpor  = 'SAPA4H'.
    lc_dc40-rcvprt  = 'LS'.
    lc_dc40-rcvprn  = 'A4HCLNT001'.
    lc_dc40-credat  = sy-datum.
    lc_dc40-cretim  = sy-uzeit.

    OPEN DATASET path FOR OUTPUT IN LEGACY BINARY MODE.
    IF sy-subrc <> 0.
      out->write( |SKIP type={ spec-idoctyp } reason=open_output_failed path={ path }| ).
      RETURN.
    ENDIF.
    TRANSFER lc_dc40 TO path LENGTH 524.

    " --- data records (EDI_DD40, 1063 bytes each) ---------------------------
    SORT lt_segments BY nr.
    TYPES: BEGIN OF ty_emit,
             segnum TYPE i,
             segtyp TYPE edi_iapi11-segmenttyp,
           END OF ty_emit.
    DATA: lt_emit   TYPE STANDARD TABLE OF ty_emit,
          lv_segnum TYPE i VALUE 0,
          lv_psgnum TYPE i,
          lc_dd40   TYPE edi_dd40.

    LOOP AT lt_segments INTO DATA(s).
      lv_segnum = lv_segnum + 1.
      " Parent link: SEGNUM of the most recent emitted record of the parent segment
      " type (parents always precede their children in the definition order).
      lv_psgnum = 0.
      IF s-parseg IS NOT INITIAL.
        LOOP AT lt_emit INTO DATA(e) WHERE segtyp = s-parseg.
          lv_psgnum = e-segnum.
        ENDLOOP.
      ENDIF.

      CLEAR lc_dd40.
      lc_dd40-segnam = s-segmenttyp.
      lc_dd40-mandt  = sy-mandt.
      lc_dd40-docnum = index.
      lc_dd40-segnum = lv_segnum.
      lc_dd40-psgnum = lv_psgnum.
      lc_dd40-hlevel = s-hlevel.
      lc_dd40-sdata  = s-segmenttyp.                " structural fill (no business data)
      TRANSFER lc_dd40 TO path LENGTH 1063.

      APPEND VALUE #( segnum = lv_segnum segtyp = s-segmenttyp ) TO lt_emit.
    ENDLOOP.
    CLOSE DATASET path.

    " --- per-type dictionary (segnam/field offsets) so callers can typed-read the
    "     flat file or convert it to IDoc-XML with erpl_idoc's sap_idoc_to_xml.
    "     Same 13-column schema as test/fixtures/flight_dict.csv. -----------------
    DATA(dpath) = |{ dir }/{ spec-idoctyp }.dict.csv|.
    OPEN DATASET dpath FOR OUTPUT IN TEXT MODE ENCODING UTF-8.
    IF sy-subrc = 0.
      DATA lv_row TYPE string.
      lv_row = `idoctyp,cimtyp,release,segnam,segdef,field_pos,field_name,offset,length,datatype,data_element,description,mandatory`.
      TRANSFER lv_row TO dpath.
      LOOP AT lt_fields INTO DATA(f).
        READ TABLE lt_segments INTO DATA(sg) WITH KEY segmenttyp = f-segmenttyp.
        DATA(segdef) = COND string( WHEN sy-subrc = 0 THEN sg-segmentdef ELSE `` ).
        lv_row = |{ spec-idoctyp },,,{ f-segmenttyp },{ segdef },{ f-field_pos },{ f-fieldname },{ f-byte_first },{ f-extlen },{ f-datatype },,,false|.
        TRANSFER lv_row TO dpath.
      ENDLOOP.
      CLOSE DATASET dpath.
    ENDIF.

    out->write( |GEN type={ spec-idoctyp } ver={ spec-version } segs={ lv_segnum } bytes={ 524 + lv_segnum * 1063 } path={ path } dict={ dpath }| ).
  ENDMETHOD.

  METHOD read_type_list.
    OPEN DATASET path FOR INPUT IN TEXT MODE ENCODING DEFAULT.
    IF sy-subrc <> 0.
      RETURN.
    ENDIF.
    DATA lv_line TYPE string.
    DO.
      READ DATASET path INTO lv_line.
      IF sy-subrc <> 0.
        EXIT.
      ENDIF.
      CONDENSE lv_line.
      IF lv_line IS INITIAL OR lv_line(1) = '#'.
        CONTINUE.
      ENDIF.
      SPLIT lv_line AT ` ` INTO DATA(lv_t) DATA(lv_v).
      TRANSLATE lv_t TO UPPER CASE.
      IF lv_v IS INITIAL.
        lv_v = '4'.
      ENDIF.
      APPEND VALUE #( idoctyp = lv_t version = lv_v ) TO rt.
    ENDDO.
    CLOSE DATASET path.
  ENDMETHOD.

  METHOD mestyp_of.
    " Best-effort message type: the basic type without its trailing release digits
    " (MATMAS05 -> MATMAS, COND_A07 -> COND_A, ORDERS05 -> ORDERS).
    rv = idoctyp.
    DATA lv_len TYPE i.
    lv_len = strlen( rv ).
    WHILE lv_len > 0.
      DATA(lv_ch) = substring( val = rv off = lv_len - 1 len = 1 ).
      IF lv_ch CO '0123456789'.
        lv_len = lv_len - 1.
      ELSE.
        EXIT.
      ENDIF.
    ENDWHILE.
    rv = substring( val = rv len = lv_len ).
  ENDMETHOD.

  METHOD default_types.
    " A broad superset of common *standard* SAP basic types. Any not installed on the
    " target system are skipped (harmless), so the same list yields a useful corpus on
    " a full S/4 (logistics types present) and on a minimal A4H trial (master-data /
    " ALE-control / retail types present). Add your own via /tmp/idoc_corpus/types.txt.
    rt = VALUE #(
      " --- master data ---
      ( idoctyp = 'MATMAS05'                      version = '4' )
      ( idoctyp = 'DEBMAS07'                      version = '4' )
      ( idoctyp = 'CREMAS06'                      version = '4' )
      ( idoctyp = 'ADRMAS03'                      version = '4' )
      ( idoctyp = 'BANK_CREATE01'                 version = '4' )
      " --- sales / purchasing / logistics ---
      ( idoctyp = 'ORDERS05'                      version = '4' )
      ( idoctyp = 'INVOIC02'                      version = '4' )
      ( idoctyp = 'DELVRY07'                      version = '4' )
      ( idoctyp = 'DESADV01'                      version = '4' )
      ( idoctyp = 'SHPMNT06'                      version = '4' )
      ( idoctyp = 'WMMBID02'                      version = '4' )
      " --- finance ---
      ( idoctyp = 'ACC_BILLING02'                 version = '4' )
      " --- ALE control / general ---
      ( idoctyp = 'ALEAUD01'                      version = '4' )
      ( idoctyp = 'ALEREQ01'                      version = '4' )
      ( idoctyp = 'TXTRAW01'                       version = '4' )
      " --- retail POS (present on many trials) ---
      ( idoctyp = 'WPUUMS01'                      version = '4' )
      ( idoctyp = 'WPUBON01'                      version = '4' )
      ( idoctyp = 'WPUWBW01'                      version = '4' )
      " --- demo type shipped with A4H ---
      ( idoctyp = 'FLIGHTBOOKING_CREATEFROMDAT01' version = '4' ) ).
  ENDMETHOD.

ENDCLASS.
