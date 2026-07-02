# erpl_idoc — README demo (real DuckDB session against test/fixtures)
# Render from repo root: vhs assets/erpl_idoc_demo.tape

Output assets/erpl_idoc_demo.gif
Output assets/erpl_idoc_demo.mp4

Set Shell "bash"
Set FontFamily "monospace"
Set FontSize 18
Set LetterSpacing 0
Set LineHeight 1.2
Set Width 1040
Set Height 660
Set Padding 28
Set Framerate 30
Set TypingSpeed 20ms
Set Theme { "name": "erpl", "background": "#000000", "foreground": "#e6edf3", "black": "#000000", "red": "#ff5f56", "green": "#7ee787", "yellow": "#fff100", "blue": "#79c0ff", "magenta": "#d2a8ff", "cyan": "#56d4dd", "white": "#e6edf3", "brightBlack": "#6e7681", "brightRed": "#ff7b72", "brightGreen": "#7ee787", "brightYellow": "#fff100", "brightBlue": "#79c0ff", "brightMagenta": "#d2a8ff", "brightCyan": "#56d4dd", "brightWhite": "#ffffff", "selection": "#264f78", "cursor": "#fff100" }

# --- hidden setup: launch the real shell with a clean prompt/mode -----------------
Hide
Type "cd /home/jr/Projects/datazoo/erpl-idoc" Enter
Type "export ASAN_OPTIONS=detect_leaks=0:detect_odr_violation=0" Enter
Type "printf '.prompt ''D ''\n.mode duckbox\n.maxwidth 80\n' > /tmp/erpl_init.sql" Enter
Type "clear" Enter
Type "./build/debug/duckdb -unsigned -init /tmp/erpl_init.sql" Enter
Sleep 2s
Ctrl+L
Sleep 400ms
Show

# --- visible demo -----------------------------------------------------------------
Type "-- SAP IDoc files, in and out of SQL — offline.  (INSTALL erpl_idoc FROM community)" Enter
Sleep 700ms
Type "LOAD erpl_idoc;" Enter
Sleep 600ms

Type "-- 1) read an IDoc file as a table" Enter
Sleep 300ms
Type "SELECT segnam, hlevel, substr(sdata,1,14) AS sdata FROM sap_idoc_read('test/fixtures/flight.idoc');" Enter
Sleep 1900ms

Type "-- 2) decode the opaque SDATA into typed columns (dictionary-driven)" Enter
Sleep 300ms
Type "SELECT airlineid, flightdate, customerid, class, passname" Enter
Type "FROM sap_idoc_read_segment('test/fixtures/flight.idoc','E1BPSBONEW','test/fixtures/flight_dict.csv');" Enter
Sleep 2300ms

Type "-- 3) write a byte-valid IDoc back out — byte-exact round trip" Enter
Sleep 300ms
Type "COPY (SELECT raw_record FROM sap_idoc_read_raw('test/fixtures/flight.idoc') ORDER BY record_index) TO 'out.idoc' (FORMAT sap_idoc);" Enter
Sleep 900ms
Type "SELECT (SELECT content FROM read_blob('out.idoc')) = (SELECT content FROM read_blob('test/fixtures/flight.idoc')) AS byte_exact;" Enter
Sleep 2100ms

Type "-- 4) or convert flat -> self-describing IDoc-XML" Enter
Sleep 300ms
Type ".mode line" Enter
Type "SELECT substr(xml,1,132) AS xml FROM sap_idoc_to_xml('test/fixtures/flight.idoc','test/fixtures/flight_dict.csv');" Enter
Sleep 3200ms
