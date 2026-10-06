CLASS zcl_erpl_xml_probe DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS zcl_erpl_xml_probe IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    " Print SAP's own XML rendering of names containing '/': a DDIC structure with
    " namespaced components, bound under a namespaced name (as an IDoc basic type would be).
    DATA ls TYPE /1bs/action_par_nested_struc.
    DATA xml TYPE string.
    DATA(srcbind) = VALUE abap_trans_srcbind_tab( ( name = '/BA1/F4_FX_CREATE01' value = REF #( ls ) ) ).
    TRY.
        CALL TRANSFORMATION id SOURCE (srcbind) RESULT XML xml OPTIONS xml_header = 'no'.
        out->write( |XML={ xml }| ).
      CATCH cx_root INTO DATA(lx).
        out->write( |ERROR={ lx->get_text( ) }| ).
    ENDTRY.
  ENDMETHOD.
ENDCLASS.
