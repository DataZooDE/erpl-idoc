CLASS zcl_erpl_idoc_e2e DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS zcl_erpl_idoc_e2e IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    " Import an erpl_idoc-written flat IDoc file and persist it, proving A4H accepts
    " a byte-valid IDoc produced by the DuckDB extension (SPEC acceptance #4).
    DATA(path) = `/tmp/erpl_idoc_e2e.idoc`.

    DATA: rc_dc40  TYPE edi_dc40,
          rc_dd40  TYPE edi_dd40,
          imp_ctrl TYPE edidc,
          imp_data TYPE STANDARD TABLE OF edidd,
          imp_dd   TYPE edidd.
    FIELD-SYMBOLS <sd> TYPE any.

    OPEN DATASET path FOR INPUT IN LEGACY BINARY MODE.
    IF sy-subrc <> 0.
      out->write( |ERROR: cannot open { path }| ).
      RETURN.
    ENDIF.
    READ DATASET path INTO rc_dc40 LENGTH DATA(l_dc).
    DO.
      READ DATASET path INTO rc_dd40 LENGTH DATA(l_dd).
      IF sy-subrc <> 0 AND l_dd = 0. EXIT. ENDIF.
      IF l_dd = 0. EXIT. ENDIF.
      CLEAR imp_dd.
      MOVE-CORRESPONDING rc_dd40 TO imp_dd.
      CLEAR imp_dd-docnum.
      APPEND imp_dd TO imp_data.
    ENDDO.
    CLOSE DATASET path.

    MOVE-CORRESPONDING rc_dc40 TO imp_ctrl.
    CLEAR imp_ctrl-docnum.
    imp_ctrl-mandt  = sy-mandt.
    imp_ctrl-idoctp = rc_dc40-idoctyp.   " flat IDOCTYP -> DB IDOCTP (the gotcha)
    imp_ctrl-direct = '2'.
    imp_ctrl-status = '64'.

    DATA: docnum   TYPE edidc-docnum,
          lv_state TYPE sy-subrc.
    CALL FUNCTION 'IDOC_INBOUND_WRITE_TO_DB'
      EXPORTING  pi_do_handle_error     = 'X'
      IMPORTING  pe_idoc_number         = docnum
                 pe_state_of_processing = lv_state
      TABLES     t_data_records         = imp_data
      CHANGING   pc_control_record      = imp_ctrl
      EXCEPTIONS idoc_not_saved = 1 OTHERS = 2.
    COMMIT WORK AND WAIT.
    out->write( |DOCNUM={ docnum }| ).

    " Read back the stored E1BPSBONEW segment and print its SDATA prefix.
    DATA: lt_v  TYPE STANDARD TABLE OF edidd,
          lt_vs TYPE STANDARD TABLE OF edids,
          ls_vc TYPE edidc.
    CALL FUNCTION 'IDOC_READ_COMPLETELY'
      EXPORTING document_number = docnum
      IMPORTING idoc_control    = ls_vc
      TABLES    int_edids       = lt_vs
                int_edidd       = lt_v
      EXCEPTIONS OTHERS = 1.
    READ TABLE lt_v INTO DATA(vdd) WITH KEY segnam = 'E1BPSBONEW'.
    IF sy-subrc = 0.
      ASSIGN COMPONENT 'SDATA' OF STRUCTURE vdd TO <sd>.
      DATA vsd TYPE c LENGTH 1000.
      vsd = <sd>.
      out->write( |SDATA={ vsd(40) }| ).
      out->write( |SEGCOUNT={ lines( lt_v ) }| ).
    ELSE.
      out->write( |SDATA=NOT_FOUND| ).
    ENDIF.
  ENDMETHOD.
ENDCLASS.
