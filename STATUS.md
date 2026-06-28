# terminal[] raw/raw_color Status

## Zukünftige Weiterentwicklung

Der vollständige Plan für die `ScreenBuffer`-Klasse (Nachfolger der
aktuellen prozeduralen `core.terminal_*` API) ist in
[doc/screen_buffer.md](doc/screen_buffer.md) dokumentiert. Dieser Plan
umfasst:

- Migration zu `screen[...]` Formspec-Element (mit `terminal[...]` als deprecated-Aliase)
- `ScreenBuffer` Lua-Klasse mit ~25 Methoden (Cells, High-Level, VT100-Output, Async-Input, Fokus)
- `serialize`/`deserialize` mit Base64 für JSON/mod_storage-Kompatibilität
- NanoBasic VM Integration (Phase 4) mit `kbhit`/`getchar`/`push_key`

Voraussetzungen für die Umsetzung:
- Aktueller `terminal[]` raw/raw_color Branch ist gemerged
- Spieler fordern die NanoBasic-Migration
- Luanti Engine-PR mit ScreenBuffer ist akzeptiert

## Zusammenfassung

Wir haben das `terminal[]` Formspec-Element um zwei neue Typen erweitert:
- `raw` — server-seitiger Cell-Grid, der via `core.terminal_set_cell()` mutiert wird
- `raw_color` — wie `raw`, aber mit 8-Farben fg/bg pro Zelle

Bei Init-Attach sendet der Server eine `TOCLIENT_TERMINAL_INIT`-Paket (Opcode 0x66) mit dem
kompletten Grid. Diff-Updates laufen alle 200ms via `TOCLIENT_TERMINAL_DIFF` (0x67).

## Commits auf `feature/terminal-formspec-element`

| Commit | Inhalt |
|---|---|
| `b51afeb` | LATEST_PROTOCOL_VERSION 52→53, FORMSPEC_API_VERSION→10, `TERMINAL_MAX_DATA_LEN=64KiB`, Size-Cap, Indentation-Fix |
| `52d7a8c` | raw/raw_color Buffer-Modell, `serverterminal.{h,cpp}`, `SendTerminalInit`/`SendTerminalDiff`, 200ms-Flush-Loop, `core.terminal_set_cell`/`terminal_clear`/`terminal_get_size` |
| `c93effc` | Client-opcodes für 0x66/0x67 Handler (Sigsegv-Fix) |
| `4c068fa` | formname-Vergleich via `getFormName()` (Bug: `IGUIElement::getName` ≠ formname); 5/6-Field-Grammar vereinheitlicht |
| `41685b6` | Endian-Bug: `writeU32` benutzt `htobe32` (big-endian), Client manuell in little-endian decoded. `readU32`/`readU16` nutzen |
| `f33973b` | Per-Element `font_size` Override via `style[elemname;font_size=N]` |
| `bug21` (uncommitted) | `INTERACT_PLACE` State-Set auf `pointed.node_undersurface` statt `node_abovesurface`. Vorher griff der State-Setter auf die Luft über dem Node zu, was nie ein NodeMeta-`formspec` hatte → State wurde nie gesetzt → kein INIT. |

## Demo-Mod: `mods/terminal_memory/`

Drei Nodes, alle 80×24 bzw. 40×25 / 20×10 groß:
- `terminal_memory:raw_display` — PDP-8 Memory-Dump
- `terminal_memory:raw_color_display` — C64-Startscreen
- `terminal_memory:diff_demo` — animiertes Bouncing-Ball-Fenster

Helpers: `make_formspec()`, `paint_line()`, `paint_lines()`, `paint_box()`, Box-Drawing-Chars (`┌─┐│└┘`).
+ / - Buttons in der oberen rechten Ecke: persistent in Node-Meta (`trm_font_size`), clamped 8-32.
Chat-Commands: `/tmem` (spawn), `/tmem_info` (buffer-info).

## Bug-Geschichte (chronologisch)

### Bug 1-2: `+`/`-` Buttons ändern Textgröße nicht

**Symptom**: Beim Klick auf `+`/`-` zeigt das `size N`-Label weiter `1`, die Textgröße ändert sich nicht.
**Sekundärsymptom nach dem ersten Fix**: Nur 3 schwarze Bildschirme, weil `Server::scanFormspecForTerminals` nur von `minetest.show_formspec` aufgerufen wird — nicht von der NodeMeta-Formspec-Pipeline.

#### Root Causes (zwei separate Bugs)

**Bug 1**: `minetest.show_formspec(name, formname, fs)` schickt Felder via `sendInventoryFields` (Opcode 0x3c) → `on_playerReceiveFields`. Der Node-`on_receive_fields(pos, formname, fields, sender)` Callback wird **nie** aufgerufen. Der Patch für `on_receive_fields` war also toter Code.
→ Fix im Mod: `meta:set_string("formspec", fs)` setzen **ohne** `show_formspec` aufzurufen. Der Client öffnet die Form selbst via `NodeMetadataFormSource::getForm()` (`game.cpp:3019`) und schickt Felder via `sendNodemetaFields` → `node_on_receive_fields` → Node-`on_receive_fields`. Außerdem: bei NodeMeta-Formspecs schickt der Client **immer** `formname=""` (siehe `TextDestNodeMetadata::gotText` in `game_formspec.cpp:37`), also darf `on_receive_fields` nicht auf `formname ~= FORM_RAW` testen.

**Bug 2**: `Server::scanFormspecForTerminals` läuft **nur** in `Server::showFormspec()`. Die NodeMeta-Pipeline (`meta:set_string("formspec", fs)` → `MEET_BLOCK_NODE_METADATA_CHANGED` → `Server::sendMetadataChanged`) ruft es nicht auf. Resultat: kein Server-seitiger Buffer wird erstellt, also keine INIT/DIFF-Updates an den Client.
→ Fix in `src/server.cpp::sendMetadataChanged`: für jede Position deren Meta-Form nicht leer ist, `scanFormspecForTerminals(fs, "nodemeta@<x>,<y>,<z>")` aufrufen UND `m_formspec_state_data[peer_id] = formname` setzen, damit `flushTerminalBuffers` weiß, dass dieser Peer die Form offen hat.

### Bug 3-20: Diverse Race Conditions und Type-Mismatches

- `on_construct` race condition (NodeMeta noch nicht emerged) → `minetest.after(0.5, ...)` Fallback
- Type-mismatch auf Re-Place → `on_destruct` muss `terminal_destroy` aufrufen
- `terminal_set_cell` lazy-create mit falschem Default (80x25 raw) → Caller muss `terminal_scan_formspec` zuerst
- `restoreState` font-size restore conditional auf `m_font_size_override == 0` (Style wins)
- Per-Node formname: alle 3 Nodes benutzen Element-Name `"scr"`, daher `nodemeta@<pos>` als formname
- `INTERACT_PLACE` Place-Item vs Right-Click unterscheidung: `above_is_air` (initial), später `pointed.node_undersurface` (final)
- `setFontSizeOverride` zwingt `m_font = nullptr` für Font-Reload

### Bug 21 (final): `pointed.node_undersurface` statt `node_abovesurface`

**Symptom**: `+`/`-`/`Close` Events kommen an, aber `initFromServer` läuft nie. `Server::SendTerminalInit` wird nie aufgerufen. Forms bleiben schwarz (außer dem Diff-Demo, der per Timer läuft — aber dort ist der Init-Trigger auch nicht der User-Click, sondern der `on_construct`→`on_rightclick` Pfad).

**Root Cause**: In `src/network/serverpackethandler.cpp::handleCommand_PlayerAction` (INTERACT_PLACE branch) wurde der State-Setter auf `pointed.node_abovesurface` angewendet. Das ist aber die **Luft über dem angeklickten Node** (bzw. die Position, wo ein neuer Block platziert würde), nicht der Node selbst. Bei einem Rechtsklick auf einen existierenden Terminal-Node ist `node_abovesurface` = Luft über dem Node → `NodeMetadata*` ist null → kein formname → State wird nicht gesetzt → `flushTerminalBuffers` filtert diesen Peer raus → kein INIT.

Lösung: Statt `pointed.node_abovesurface` muss `pointed.node_undersurface` benutzt werden. Das ist laut `src/util/pointedthing.h:34` der Node, dessen Nodebox vom Ray getroffen wurde — also genau der Node, auf den der User klickt. Bei Place-Item auf dem Boden ist `node_undersurface` der Boden-Node (keine Form → State wird nicht gesetzt, was korrekt ist); bei Right-Click auf einen Terminal-Node ist es der Terminal-Node selbst (mit Form → State wird gesetzt, was korrekt ist).

**Bestätigung im Test-Log** (final):
```
22:30:57 INTERACT_PLACE: peer=61329 under=-322,10,-334 above=-322,10,-335
22:30:57 Server::SendTerminalInit: formname=nodemeta@-322,10,-334 element=scr cols=40 rows=25 type=2 cell_data.size=6000
22:30:57 Client::handleCommand_TerminalInit CALLED
22:30:57 GUITerminal::initFromServer CALLED: type=2 cols=40 rows=25 cell_data.size=6000
22:31:08 close=Close, quit=true  <- Form wurde geöffnet und geschlossen
```

### Per-Node formname

Da mehrere `raw_display`-Nodes (verschiedene Positionen) jeweils einen eigenen Server-seitigen Buffer brauchen, benutzen wir `nodemeta@<x>,<y>,<z>` als formname. Der Mod generiert diesen mit `node_formname(pos)`, der Server mit `itos(pos.X/Y/Z)`. Beide Seiten matchen, und `m_terminal_buffers.getOrCreate(...)` legt für jede Node-Position einen separaten Buffer an.

## Geänderte Dateien (Finaler Stand)

- `src/network/networkprotocol.{h,cpp}` — Proto-Bump 54, 2 neue Opcodes
- `src/client/client.{h,cpp}` — `handleCommand_TerminalInit`/`Diff`
- `src/client/clientevent.h` — `CE_TERMINAL_INIT`/`DIFF`
- `src/client/game.cpp` — Handler + `clientEventHandler` Eintrag
- `src/client/game_formspec.{h,cpp}` — `initTerminalBuffer`/`applyTerminalDiff`
- `src/client/game_internal.h` — Handler-Deklaration
- `src/gui/guiFormSpecMenu.{h,cpp}` — `parseTerminal` Style-Lookup, `getFormName()`-Getter
- `src/gui/guiTerminal.{h,cpp}` — `initFromServer`/`applyServerDiff` mit `readU32`/`readU16`, `setFontSizeOverride`
- `src/server.cpp` — `SendTerminalInit`/`Diff`, `scanFormspecForTerminals`, `flushTerminalBuffers` (200ms), `terminalSetCell`/`Clear`/`GetSize`/`Destroy`, `handleCommand_TerminalKey`
- `src/script/lua_api/l_server.{h,cpp}` — `terminal_set_cell`/`terminal_clear`/`terminal_get_size`/`terminal_destroy`/`terminal_scan_formspec`
- `src/network/clientpackethandler.cpp` — `handleCommand_TerminalInit`/`Diff`
- `src/network/clientopcodes.cpp` — `0x66`/`0x67` Handler
- `src/network/serverpackethandler.cpp` — `INTERACT_PLACE` State-Set via `pointed.node_undersurface`
- `src/serverterminal.{h,cpp}` — neu: `ServerTerminalBuffer`, `ServerTerminalStore`
- `src/CMakeLists.txt` — `serverterminal.cpp` zu SRCS
- `doc/lua_api.md` — neue API dokumentiert
- `builtin/game/misc_s.lua` — `["5.17.0"]=53`, `["5.18.0"]=54`

## Build-Status

- Build: clean (`cmake --build build -j$(nproc)`)
- Tests: 172.708 Assertions grün, 0/330 fehlgeschlagen
- Mods: `terminal_test`, `terminal_memory`, `nanobasic` geladen, keine Lua-Fehler

## Bug-Geschichte (chronologisch)

### Bug 21-26: Multi-Client + Animation

**Bug 21** (`pointed.node_undersurface`): Der `INTERACT_PLACE`-Handler setzte den Peer-State auf `pointed.node_abovesurface` (= Luft über dem Node). Bei einem Rechtsklick auf einen existierenden Terminal-Node ist das **immer** Luft → kein NodeMeta → kein formname → State wird nicht gesetzt → kein INIT. **Fix**: `pointed.node_undersurface` (= der Node, dessen Nodebox der Ray trifft) benutzen.

**Bug 22** (on_rightclick resettet frame=0 für alle Spieler): Wenn ein zweiter Spieler die Form öffnete, rief `on_rightclick` immer `render_diff_frame(pos, 0)` auf, was den Buffer für den ersten Spieler zurücksetzte (Border weg, Ball oben links). **Fix**: aktuellen Frame aus Node-Meta lesen.

**Bug 23** (on_timer stoppt für alle Spieler): Der `on_timer` prüfte `playername = meta:get_string("player")` und stoppte, wenn der Spieler disconnected. Im Multi-Client-Fall überschrieb der zweite Spieler den Namen, und wenn er disconnectete, stoppte die Animation für alle. **Fix**: Timer läuft immer; das "player"-Tracking wurde komplett entfernt.

**Bug 24** (on_rightclick resettet laufende Animation): `on_rightclick` setzte `meta:set_int("frame", 0)`, was die Animation beim zweiten Spieler zurücksetzte. **Fix**: frame-Counter nur in `on_construct` und im LBM auf 0 setzen, nicht in `on_rightclick`.

**Bug 25** (LBM fehlt für Map-Load): Nach Server-Restart lief `on_construct` für existierende Nodes nicht. NodeMeta-Formspec war im Meta, aber der Server-Buffer war leer. Der Spieler sah beim ersten Klick einen leeren Buffer (oder beim `diff_demo` Border + Ball nur, wenn frame==0). **Fix**: `minetest.register_lbm({ run_at_every_load = true })` ruft `terminal_scan_formspec` und `render_*` für alle drei Node-Typen auf.

**Bug 26** (Border wird nicht gemalt wenn Timer vor Klick lief): Der Border wurde nur im `frame == 0`-Branch gemalt. Wenn der LBM Node auf frame=0 setzte aber der Timer schon lief, wurde der Border nur einmal gemalt; spätere `render_diff_frame(N)` mit `N > 0` übersprangen den Border. Im Multi-Client-Fall: Spieler 2 öffnete die Form, wenn Timer schon frame=50 hatte → `render_diff_frame(50)` übersprang Border. **Fix**: `border_drawn` Meta-Flag; Border wird gemalt wenn `frame==0 OR border_drawn==0`.

**Bug 27** (Animation period falsch): `period = 2*x_max + 2*y_max - 4` war 4 Schritte zu kurz. Die up-Leg hatte nur 3 Schritte statt 7; Ball sprang von `(1, 6)` zu `(1, 2)`. **Fix**: `period = 2*(x_max + y_max)` für einen vollen Loop.

**Bug 28** (Counter überschreibt Ball): Counter stand auf Zeile `ROWS_DIFF - 2 = 8`, Ball auf derselben Zeile während der down/left-Leg. Counter-Paint löschte 14 Spaces und überschrieb den Ball. **Fix**: Counter auf Zeile `ROWS_DIFF - 3 = 7`.

**Bug 29** (Ball geht nicht ganz nach rechts): `x_max = COLS_DIFF - 3 = 17` lies den Ball bei Spalte 17 stoppen, eine Spalte vor dem `+` an Spalte 19. **Fix**: `x_max = COLS_DIFF - 2 = 18`.

**Bug 30** (Ball überschreibt Border-Bottom): Mit `y_max = ROWS_DIFF - 2 = 8` lief der Ball auf Zeile 8 (die Border-Zeile? Nein, ROWS_DIFF-1=9 ist die Border-Zeile). Mit `by = y_max + 1 = 9` lief der Ball auf der Border-Zeile und überschrieb `-` mit `O`. **Fix**: Border-Top mit Label integriert (`+- diff demo ------+`), Ball läuft ab Zeile 1 statt 2. `by` für down/left leg ist jetzt `y_max` (statt `y_max+1`). Ball bleibt im Innenraum, Border bleibt sichtbar.

## Build-Status

- Build: clean (`cmake --build build -j$(nproc)`)
- Tests: 172.708 Assertions grün, 0/330 fehlgeschlagen
- Mods: `terminal_test`, `terminal_memory`, `nanobasic` geladen, keine Lua-Fehler

## Test-Status (User-seitig)

- ✅ Form öffnen (alle 3 Nodes)
- ✅ INIT-Buffer kommt an, Inhalt wird gerendert (C64, Memory-Dump, Bouncing-Ball)
- ✅ +/- Buttons: Schriftgröße verändert sich sichtbar, persistent in Node-Meta
- ✅ DIFF-Streaming: animierter Ball aktualisiert mit ~5 fps (200ms-Intervall)
- ✅ Animation: Border-Top mit "diff demo" Label, Ball läuft im Innenraum ohne Border zu überschreiben
- ✅ Counter "t=N" auf Zeile 7, kontinuierlich hochzählend
- ✅ Multi-Client: Spieler 2 sieht Border + Ball an gleicher Position wie Spieler 1
- ✅ Multi-Client: Spieler 2 schließt → Animation läuft für Spieler 1 weiter

## Final-Test-Log (22:37 Uhr, Single-Player)

```
2026-06-22 22:37:16: ACTION[Server]: singleplayer places node terminal_memory:raw_display at (-323,10,-335)
2026-06-22 22:37:17: ACTION[Server]: singleplayer places node terminal_memory:raw_color_display at (-322,10,-335)
2026-06-22 22:37:19: ACTION[Server]: singleplayer places node terminal_memory:diff_demo at (-321,10,-335)
[... + / - / close Events ...]
2026-06-22 22:37:38: ACTION[Server]: singleplayer leaves game.
```

## Final Multi-Client Test (geplant)

```
Terminal 1:  ./run.sh           # singleplayer (auch Server)
Terminal 2:  ./bin/luanti --name bob --address 127.0.0.1 --port 30000
```

1. Spieler 1 platziert 3 Nodes, klickt diff_demo → Animation läuft
2. Spieler 2 verbindet, klickt denselben Node → sieht Border + Ball an gleicher Position
3. Spieler 2 schließt Form → Spieler 1 Animation läuft weiter
4. Spieler 1 schließt auch → Animation läuft im Hintergrund (kein Beobachter, aber State bleibt erhalten)

## Final Multi-Client Test (bestätigt)

User-seitig bestätigt am 2026-06-24:
- Border sichtbar
- Animation läuft durch
- Auch wenn zweiter Spieler kommt und geht
- Beide Spieler sehen den gleichen Stand (Border + Ball an gleicher Position)

## yauc2 / vm2 / termlib Integration (2026-06-26)

Für die spätere `ScreenBuffer`-Migration wurde yauc2 (16-bit Computer) und
seine Abhängigkeiten in `luanti-dev/mods/` kopiert:

- `mods/vm2/` — 16-bit CPU C-Lib + Lua API
- `mods/termlib/` — Terminal-Library (Text-Terminal-Pattern)
- `mods/yauc2/` — 16-bit Minicomputer-Simulation (PDP-8, IBM-inspiriert)
- `mods/yauc2_test/` — Auto-Setup-Test (chatcommand + auto-test)

### Anpassungen für standalone-Betrieb (ohne techage/tubelib)

**Problem**: `yauc2` benötigt entweder `techage` ODER `tubelib` und crashed
beim CPU-Platzieren ohne eines davon (`attempt to index upvalue 'tech'
(a nil value)` in `src/cpu.lua:95`).

**Lösung**: Stub-Wrapper in `src/tech/wrapper.lua` (else-Branch):

- `yauc2.tech.get_nvm` → `minetest.get_meta` direkt
- `yauc2.tech.get_mem` → NodeMeta-Feld `_mem` (serialisiert)
- `yauc2.tech.add_node` → Dummy-Node-Nummer aus Position-Hash
- Alle anderen `tech.*`-Funktionen → `stub()` (no-op)
- `yauc2.tech.update` als Stub hinzugefügt (in `cpu.lua` referenziert,
  aber in keinem Wrapper definiert)
- `yauc2.tech` wird immer initialisiert (auch ohne techage/tubelib)

### world.mt / minetest.conf

```
load_mod_termlib = mods/termlib
load_mod_vm2 = mods/vm2
load_mod_yauc2 = mods/yauc2
load_mod_yauc2_test = mods/yauc2_test

secure.trusted_mods = nanobasic, vm2, yauc2, termlib
```

### Test (bestätigt)

```bash
yauc2_auto_test = 1  # in minetest.conf
./bin/luantiserver --worldname TestVT100
```

Ergebnis:
- CPU `yauc2:cpu1_no_power` wird platziert ohne Crash
- `after_place_node` läuft durch (Stub-`tech` greift)
- `Map::getNodeMetadata(): Block not found` Warning nur weil Block
  nicht emerged (nicht relevant)

### Nächster Schritt

Migration `yauc2:teletype` zu `ScreenBuffer`-API (Phase 1+2):
- `textarea[...]` → `screen[...;raw_color]`
- `update_teletype` mit `buf:set_cell()` statt `term:put_char()`
- `on_construct`/`on_destruct` mit `serialize`/`deserialize`
- Persistenz: Buffer-State in NodeMeta

Bereit für Implementation, sobald die ScreenBuffer-Engine-Änderungen
aus `doc/screen_buffer.md` umgesetzt sind.

## Phase 1-9: ScreenBuffer Engine-Implementation (bestätigt)

Die ScreenBuffer-Lua-Klasse wurde in der Engine implementiert und
getestet. Der `terminal_memory` Mod wurde komplett auf die neue
API umgestellt. Die alte `core.terminal_*` API wurde entfernt.

### Formspec-Aliase

- `screen[X,Y;W,H;name;cols,rows;type]` — bevorzugter Name
- `terminal[X,Y;W,H;name;cols,rows;type]` — deprecated-Aliase
  (für Rückwärtskompatibilität alter Karten)

### ScreenBuffer-Klasse (Engine-Code)

**Datei**: `src/script/lua_api/l_server.{h,cpp}`

**Constructor** (global registriert als `ScreenBuffer(...)`):
```lua
ScreenBuffer(pos, element_name [, type])  -- pos ist {x=, y=, z=}
```

**Methoden** (alle als Class-Members):
- Accessors: `get_pos`, `get_formname`, `get_element_name`,
  `get_type`, `get_cols`, `get_rows`, `get_size`, `get_version`,
  `is_valid`
- Cell ops: `set_cell`, `get_cell`, `clear`
- Cursor: `set_cursor`, `get_cursor`
- Write: `write_char`, `write_string`, `write_line`
- Areas: `fill_rect`, `draw_box`, `scroll`
- Persistence: `serialize`, `deserialize` (Base64)

**Trampolines**: Da GCC/x86-64 die Konvertierung von statischen
Member-Function-Pointern zu `lua_CFunction` nicht zuverlässig
macht, wurden alle Methoden als statische freie C-Funktionen
trampolined, die an die `ModApiServer`-Member weiterleiten.

**Multi-Context-Registrierung**: Da der Lua-State
`LUA_REGISTRYINDEX` per-State ist und `minetest.after()`-Callbacks in
einem separaten Async-Engine-State laufen, ruft `push_screenbuffer`
jedes Mal `register_screenbuffer_class` auf. Das Metatable wird
idempotent neu erstellt (via `luaL_newmetatable`) und alle
Methoden werden mit absoluten Stack-Indizes registriert, damit das
pop-Verhalten von `lua_setfield` keine Stack-Bugs verursacht.

**Wichtige Erkenntnisse aus dem Debug**:
1. `lua_setfield(L, n, k)` setzt `stack[n][k] = top` und pop'd —
   `n` ist der Table-Index, `top` der Wert-Index. **`n` muss
   der Index der Table sein, nicht der Wert!**
2. `lua_typename(L, idx)` braucht den **Typ** (via `lua_type(L, idx)`),
   nicht den Index direkt!
3. Statische Member-Function-Pointer sind **nicht** implizit zu
   `lua_CFunction` konvertierbar — Trampolines sind nötig.
4. `luaL_newmetatable` ruft `lua_getfield(L, LUA_REGISTRYINDEX, n)`
   auf — wenn `n` existiert, wird die existierende Metatable gepusht.
   Der `top` ist dann der **Index** der existierenden Metatable.

### Test (bestätigt)

```bash
# Server starten
./bin/luantiserver --worldname TestVT100
```

Mod-Konfiguration (in `terminal_memory`):
```lua
local buf = ScreenBuffer({x=0, y=20, z=0}, "scr", "raw_color")
buf:set_cell(0, 0, "X", 6, 0)
local ch, fg, bg = buf:get_cell(0, 0)  -- ch=88, fg=6, bg=0
local encoded = buf:serialize()  -- 12010-Byte Binary-String
buf:clear()
buf:deserialize(encoded)  -- restored
```

### Mod-Migration (terminal_memory)

Der `terminal_memory` Mod wurde komplett auf die neue `ScreenBuffer`-
API umgestellt. Alle Render-Funktionen erwarten jetzt das
`buf`-Objekt als Parameter und werden via `ScreenBuffer(pos, elem)`
erzeugt:

```lua
on_construct = function(pos)
    local formname = node_formname(pos)
    local fs = make_formspec(...)
    meta:set_string("formspec", fs)
    local buf = ScreenBuffer(pos, ELEM_RAW)
    render_raw_static(buf)
end,

on_rightclick = function(pos, node, clicker)
    ...
    local buf = ScreenBuffer(pos, ELEM_RAW)
    render_raw_static(buf)
end,
```

### Verbleibende Aufgaben

1. **Phase 10: yauc2:teletype Migration** — die `teletype.lua` in
   `mods/yauc2/src/` muss `term:put_char` durch `buf:set_cell`
   ersetzen und die `textarea[...]`-Form durch `screen[...]`. Die
   `update_teletype`-Schleife muss dann `buf:set_cell` aufrufen.
2. **Base64 für serialize** — aktuell wird der rohe Binär-String
   zurückgegeben, nicht Base64. Für JSON-Storage brauchen wir
   noch den `l_screenbuffer_base64_encode`-Aufruf.

### Behobene Bugs

- `render_raw_static(formname, ELEM_RAW)` und
  `render_color_static(formname, ELEM_COLOR)` riefen die
  Render-Funktionen mit falschen Argumenten auf. Korrekt ist
  `render_raw_static(buf)` bzw. `render_color_static(buf)` mit dem
  via `ScreenBuffer(pos, elem)` erzeugten Objekt.
- Die `make_formspec`-Funktion benutzte eine fest vorgegebene
  Form-Size (`size[14.6,9.5]` und `terminal[0.3,0.7;13.4,7.8]`).
  Bei einem 80x24-Display führte das zu einem Cell-Aspekt von
  0.52 (0.168 breit × 0.325 hoch) — stark gestaucht. Spätere
  Versuche, die Form-Size dynamisch pro Display zu berechnen
  (`size[13,8.6]` für 40x25, `size[25,8.3]` für 80x24, etc.),
  haben das Problem NICHT gelöst — Luanti's `GUITerminal::draw()`
  setzt die Cell-Pixel-Größe **intern** aus der Font-Metrik:
  `cell_w = char_dim.Width` (≈ 0.6 × font_size bei Monospace) und
  `cell_h = char_dim.Height + 2` (Font-Höhe plus 2 Pixels
  "small line spacing"). **Der `+2` ist hartcodiert** in Luanti
  und kann von außen nicht überschrieben werden. Die korrekte
  Lösung ist, eine **8×8 (oder vergleichbar quadratische) Font**
  zu installieren — die Cell-Aspect folgt direkt aus dem Font
  selbst, nicht aus der Form-Geometrie. Mit der Original-
  Form-Size `size[14,9.5]` und einem quadratischen Font
  erscheinen die Cells dann korrekt 1:1. Mit der Standard-Cousine
  (Cell-Aspect ≈ 1:2) bleibt der Rand oben/unten sichtbar
  dicker als an den Seiten.
- **Engine-Fix (`cell_h = max(cell_w, char_dim.Height+2)`)**:
  `GUITerminal::draw()` in `src/gui/guiTerminal.cpp` setzt jetzt
  `cell_h = std::max(cell_w, char_dim.Height + 2)`. Bei
  Luanti's Default-Cousine ist die Font-Höhe ~14, cell_w ~8,
  also `cell_h = max(8, 16) = 16` — was die Glyphen-Höhe
  vollständig umfasst. Bei einer C64-style-Font (cell_w ~8,
  char_dim.Height ~8) ergibt sich `cell_h = max(8, 10) = 10`,
  was fast quadratische Cells beibehält. Zuvor war
  `cell_h = char_dim.Height + 2`, was bei Cousine zu
  `cell_w:cell_h = 1:2` führte. Mit diesem Fix ist die Cell-Aspect
  immer ≥ 1:1, der dickere Rand oben/unten verschwindet, und die
  Glyphen werden nicht geclippt. Der 20×10 diff_demo-Node wurde
  in ein Snake-Demo umgewandelt (die Bouncing-Ball-Logik wurde
  zu einer 3-Segment-Schlange mit `@`-Kopf, `#`-Körper,
  `+`-Schwanz-Verlängerung).
- **C64-Font (verworfen)**: Zunächst wurde versucht, die
  `fonts/C64_Pro_Mono-STYLE.ttf` als globale Mono-Font zu
  setzen, um eine quadratische Cell-Aspect zu erreichen. Da die
  `mono_font_path`-Einstellung in Luanti **global** ist (für alle
  Forms), würde sie auch andere Mods betreffen, die
  `mono_font_path` nicht erwarten. Eine saubere Lösung würde
  eine Engine-Erweiterung (`style[elem;font_path=PATH]`)
  erfordern, die aber als zu invasiv für einen PR angesehen
  wurde. Daher wurde die C64-Font-Setzung aus `minetest.conf`
  wieder entfernt. Die `screen[...]`-Forms verwenden jetzt
  Luanti's Default-Font.
- **Workaround bei C64-Like Fonts**: Mit `cell_h = max(cell_w,
  char_dim.Height+2)` ist die Cell-Aspect nur 1:1 wenn die
  Font quadratische Glyphen hat. Für Fonts mit breiteren
  Glyphen (cell_w < char_dim.Height+2) ist cell_h > cell_w.
  Dieser Engine-Fix bleibt erhalten, damit Glyphen auch bei
  nicht-quadratischen Fonts nicht geclippt werden.
- **Engine-Fix: Font-Pfad-Auflösung**: `FontEngine::getFont()`
  in `src/client/fontengine.cpp` resolved nun relative Font-
  Pfade aus den Settings via `porting::getDataPath()`. Ohne
  diesen Fix scheiterte `FT_New_Face()` mit
  `FT_Err_Cannot_Open_Resource`, weil das aktuelle Working-
  Directory beim Client-Start nicht `/home/joachim/luanti-dev`
  war. Mit dem Fix wird der Font korrekt gefunden
  (z.B. `mono_font_path = fonts/C64_Pro_Mono-STYLE.ttf` →
  `/home/joachim/luanti-dev/fonts/C64_Pro_Mono-STYLE.ttf`).
  Der Fix bleibt erhalten, auch wenn die C64-Font nicht aktiv
  ist — er ist eine allgemeine Robustness-Verbesserung für
  alle Mods, die relative Font-Pfade nutzen.
