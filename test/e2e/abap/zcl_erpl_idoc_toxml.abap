CLASS zcl_erpl_idoc_toxml DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS zcl_erpl_idoc_toxml IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    " Read the flat IDoc file erpl_idoc wrote, convert it to IDoc-XML with the
    " standard FM IDX_IDOC_TO_XML, and write the XML to /tmp/flight.xml.
    DATA(in_path)  = `/tmp/flight.idoc`.
    DATA(out_path) = `/tmp/flight.xml`.

    DATA: dc40 TYPE edi_dc40,
          dd40 TYPE edi_dd40,
          dd40_tab TYPE STANDARD TABLE OF edi_dd40.

    OPEN DATASET in_path FOR INPUT IN LEGACY BINARY MODE.
    IF sy-subrc <> 0.
      out->write( |ERROR: cannot open { in_path }| ).
      RETURN.
    ENDIF.
    READ DATASET in_path INTO dc40 LENGTH DATA(l_dc).
    DO.
      READ DATASET in_path INTO dd40 LENGTH DATA(l_dd).
      IF sy-subrc <> 0 AND l_dd = 0. EXIT. ENDIF.
      IF l_dd = 0. EXIT. ENDIF.
      APPEND dd40 TO dd40_tab.
    ENDDO.
    CLOSE DATASET in_path.
    out->write( |read control tabnam={ dc40-tabnam } idoctyp={ dc40-idoctyp } data_recs={ lines( dd40_tab ) }| ).

    DATA xml TYPE string.
    CALL FUNCTION 'IDX_IDOC_TO_XML'
      EXPORTING
        def_typ      = 'E'
        edidc40      = dc40
        rfc_codepage = '4110'
        syntax_check = ' '
        tunnel       = ' '
        multi        = ' '
      IMPORTING
        xml_data     = xml
      TABLES
        idoc_data_40 = dd40_tab
      EXCEPTIONS
        OTHERS       = 1.
    IF sy-subrc <> 0.
      out->write( |IDX_IDOC_TO_XML failed subrc={ sy-subrc }| ).
      RETURN.
    ENDIF.

    " write the XML (UTF-8) to disk
    DATA xml_x TYPE xstring.
    DATA(conv) = cl_abap_conv_codepage=>create_out( codepage = `UTF-8` ).
    xml_x = conv->convert( xml ).
    OPEN DATASET out_path FOR OUTPUT IN BINARY MODE.
    TRANSFER xml_x TO out_path.
    CLOSE DATASET out_path.
    out->write( |wrote XML len={ strlen( xml ) } chars to { out_path }| ).
    out->write( |head: { xml(120) }| ).
  ENDMETHOD.
ENDCLASS.
