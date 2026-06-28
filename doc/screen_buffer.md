# ScreenBuffer Class — Design Plan

## Overview

This document captures the design plan for the `ScreenBuffer` Lua class
that replaces the existing procedural `core.terminal_*` API for the
`terminal[]` Formspec element (raw, raw_color, and vt100 variants).

The class follows Luanti's existing `ItemStack`, `NodeMetaRef`, `AreaStore`
conventions and adds first-class support for:

- A persistent, per-node server-side character grid
- High-level drawing operations (`write_string`, `draw_box`, `fill_rect`)
- Base64 persistence (`serialize`/`deserialize`)
- Multi-client synchronization (auto-DIFF on every cell change)
- Asynchronous input (callback-based, never blocking)
- A focus model (first-come-first-served keyboard ownership)
- Optional NanoBasic VM integration (see Phase 4)

---

## 1. Naming Convention

To avoid the confusion between the Formspec element name and the Lua
class name (e.g. `terminal[...]` vs `TerminalBuffer`), the unified name
is **`screen`**:

| Context | Before (current) | After (planned) |
|---|---|---|
| Formspec element | `terminal[X,Y;W,H;name;cols,rows;type]` | `screen[X,Y;W,H;name;cols,rows;type]` |
| Lua class | `ScreenBuffer` (new) | `ScreenBuffer` (unchanged) |
| Mod API | `core.terminal_*` (deprecated/removed) | `ScreenBuffer` methods (only) |

For backwards compatibility with existing maps that contain the old
`terminal[...]` formspec text, the Formspec parser accepts both names
during a transition period. New mods should use `screen[...]`.

---

## 2. Formspec Element

### Syntax

```lua
screen[X,Y;W,H;name;cols;rows;type]
```

### Type Values

| Type | Description |
|---|---|
| `"raw"` | Server-driven, monospace, no color |
| `"raw_color"` | Server-driven, monospace, 8-color fg+bg per cell |
| `"vt100"` | Client emulator with cursor and ANSI escape support |

### Example

```lua
formspec_version[12]
size[14,8]
screen[0.5,0.8;13,6;scr;80;24;vt100]
button[0.5,7;1,0.5;quit;Close]
```

### Backwards Compatibility

`terminal[...]` is parsed as an alias of `screen[...]`. The Lua class
returned to mods is still `ScreenBuffer` regardless of which formspec
syntax was used to declare the element. The existing examples in
`mods/terminal_memory/init.lua` will continue to work without
modification (they declare a 5- or 6-field grammar with an explicit
type, and the new parser accepts both).

---

## 3. Lua Class: `ScreenBuffer`

### Construction

```lua
-- Per-node: bind to a node position
local buf = ScreenBuffer(pos, "scr")            -- type scanned from formspec
local buf = ScreenBuffer(pos, "scr", "raw_color")  -- explicit type
```

There is no `ScreenBuffer(formname, ...)` constructor. Every
`ScreenBuffer` is bound to a node position. This is a deliberate
simplification: it removes the "where does the buffer live?" question
and makes per-node state (cursor, focus, input queue) natural.

`deserialize` is a method on an existing instance, not a constructor.

### Read Accessors

```lua
buf:get_pos()                    -- v3s16
buf:get_element_name()           -- string
buf:get_formname()               -- "nodemeta@<x>,<y>,<z>"
buf:get_type()                   -- "raw" | "raw_color" | "vt100"
buf:get_cols()                   -- number
buf:get_rows()                   -- number
buf:get_size()                   -- cols, rows
buf:get_version()                -- u32, DIFF counter, increments on every write
buf:is_valid()                   -- false if the node was removed
```

### Cell Access (low-level)

```lua
buf:set_cell(col, row, char [, fg, bg])   -- char is single-character UTF-8
buf:get_cell(col, row)                     -- returns codepoint, fg, bg
buf:clear()                                -- reset all cells to space
```

### High-Level Drawing

These are the methods that make `ScreenBuffer` more useful than the
old procedural API:

```lua
-- Cursor
buf:set_cursor(col, row)
buf:get_cursor()                           -- col, row

-- Character/string writing with auto-cursor advancement
buf:write_char(char [, fg, bg])            -- advance after writing
buf:write_string(text [, fg, bg])          -- multi-char, wraps at right edge
buf:write_line(row, col, text [, fg, bg])  -- absolute position, no wrap

-- Areas
buf:fill_rect(x0, y0, x1, y1, char [, fg, bg])
buf:scroll(lines)                          -- positive=up, negative=down, caps at +/-rows
buf:draw_box(x0, y0, x1, y1, style [, fg, bg])
                                            -- style: "single"|"double"|"heavy"|"rounded"|"ascii"
```

### VT100 Output (the `write` method)

`buf:write(text)` is the central method for ANSI/VT100 output. It
parses a byte stream and applies it to the buffer's cursor state:

| Sequence | Effect |
|---|---|
| printable byte | `set_cell` at cursor, advance |
| `\n` | cursor to next row, col 0, scroll if past bottom |
| `\r` | cursor to col 0 of current row |
| `\t` | advance to next multiple of 8 |
| `\x1b[y;xH` / `\x1b[y;x f` | CUP: set cursor to (x, y), 1-indexed |
| `\x1b[2J` | ED: clear screen, home cursor |
| `\x1b[H` | cursor to (1, 1) |
| `\x1b[2K` | EL: clear current line |
| `\x1b[<n>m` | SGR: set attributes (bold, reverse, color) — future |

This makes the buffer a fully functional VT100 emulator, sufficient
for a NanoBasic-style BASIC REPL, retro games, and similar use cases.

### Persistence (Base64)

```lua
buf:serialize()          -- returns Base64 string, suitable for JSON / mod_storage
buf:deserialize(s)       -- restore from Base64, returns true on success
```

The serialized format is:

```
[magic:1][type:1][cols:2][rows:2][cursor_x:2][cursor_y:2]
[cell_data: cols*rows * cell_size bytes]
```

where `cell_size` is 4 for `raw` and 6 for `raw_color`. VT100 state
is reduced to the cursor position (SGR state stays client-side).

Input queue and focus owner are NOT serialized — they are transient.

### Sync

```lua
buf:set_sync(true | false)   -- default true
buf:flush()                  -- force immediate DIFF broadcast
buf:get_dirty_count()        -- number of cells modified but not yet sent
```

The engine flushes dirty cells every 200 ms by default. Disabling
sync is useful for bulk loading from `mod_storage` — set `set_sync(false)`,
fill the buffer, then `set_sync(true); flush()` to send one DIFF
instead of one per cell.

### Input (Asynchronous, Never Blocking)

Input is exposed via **callback only**. There are no blocking
`getchar`/`getline` methods, because the Luanti Lua state is shared
across all mods and the server's main thread:

```lua
buf:on_key(function(player, byte) ... end)
```

`byte` is a raw byte string (e.g. `"a"`, `"\r"`, `"\x1b[A"` for the up
arrow, see `register_on_terminal_key` for the full encoding).

The engine handles focus internally. The callback is only invoked
when the *same* peer holds focus on the buffer. Other peers'
keystrokes are dropped on the server side and the client shows a
"Read-only" indicator.

### Focus

The engine tracks a single `focus_owner` (playername) per buffer.
Whoever clicks the `screen[]` element first gets focus; others
get a denied response. Focus is released when:

- The owning peer closes the form
- The owning peer disconnects
- The owning peer is idle for longer than 60 s (configurable later)
- A mod explicitly calls `buf:release_focus()`

```lua
buf:has_focus()             -- (client-side) is this peer the focus owner?
buf:request_focus()         -- (client-side) ask the server for focus
buf:release_focus()         -- release focus explicitly
buf:get_focus_owner()       -- (server-side) playername or nil
```

---

## 4. Engine Architecture

### Per-Buffer State

The existing `ServerTerminalBuffer` (in `src/serverterminal.{h,cpp}`)
already holds the cell grid. It is extended with:

```cpp
struct ServerScreenBuffer {
    // Cell storage (existing, unchanged)
    ServerTerminalBuffer cells;

    // Input queue (new)
    std::deque<u8> input_queue;
    std::mutex input_mutex;

    // Focus (new)
    std::string focus_owner;
    u32 focus_taken_at_ms;

    // Cursor state (new, replaces C-side xpos/ypos)
    u16 cursor_x = 0;
    u16 cursor_y = 0;

    // Attributes (new, for VT100 SGR)
    u8 current_fg = 7;  // white
    u8 current_bg = 0;  // black
    bool bold = false;
    bool reverse = false;
};
```

The `ServerScreenBuffer` is owned by the `ServerTerminalStore` (the
global `m_terminal_buffers` map keyed by `nodemeta@<x>,<y>,<z>/elem`).

### Wire Protocol

The existing opcodes are renamed/extended:

| Opcode | Direction | Purpose |
|---|---|---|
| `TOCLIENT_SCREEN_INIT` (0x66) | S → C | formname, elem, type, cols, rows, version, cells |
| `TOCLIENT_SCREEN_DIFF` (0x67) | S → C | formname, elem, from_version, to_version, dirty cells |
| `TOCLIENT_SCREEN_FOCUS` (0x68) | S → C | formname, elem, granted (bool), owner (string) |
| `TOSERVER_SCREEN_INPUT` (0x68) | C → S | formname, elem, byte(s) |

`TOCLIENT_SCREEN_INIT` and `TOCLIENT_SCREEN_DIFF` reuse the existing
binary formats (u32 big-endian codepoints, optional u8 fg/bg per cell).
The wire size is unchanged; only the opcode names are renamed.

The `m_formspec_state_data[peer]` map (per-peer, per-formname state)
is extended with a `focus_owner` field per `(formname, peer)`.

### Focus Flow

1. Client opens a form with `screen[...;scr;...;vt100]`. No focus yet.
2. Client clicks the screen element. The client sends a focus request.
3. Server checks `focus_owner` for the formname:
   - Empty → grant focus, broadcast `TOCLIENT_SCREEN_FOCUS(granted=true)`.
   - Non-empty → deny, send `TOCLIENT_SCREEN_FOCUS(granted=false, owner="bob")`.
4. On grant, the client enables keystroke capture and starts sending
   `TOSERVER_SCREEN_INPUT` packets.
5. On deny, the client disables keystroke capture and shows a
   "Read-only — bob is typing" indicator (visualized via a styled
   border on the screen element).
6. Auto-release happens on form close, disconnect, or 60 s timeout.

### Lifecycle Hooks

- `Server::nodeDelete(pos)` calls
  `LuaScreenBuffer::invalidate(pos, elem)` for every `elem` known to
  the node's formspec. The Lua-side userdata is marked invalid and
  any subsequent method call returns `false` or `nil`.
- The `screen[...]` formspec parser stores the list of declared
  element names in node meta so that `nodeDelete` can iterate them.

### Formspec Parser Update

The existing parser in `src/gui/guiFormSpecMenu.cpp::parseTerminal`
accepts:

```
screen[X,Y;W,H;name;cols,rows;type]
```

with `type` being `"raw"`, `"raw_color"`, or `"vt100"`. The 5-field
form `screen[X,Y;W,H;name;cols,rows]` defaults to `"vt100"`.

The parser also accepts `terminal[...]` as a deprecated alias to
preserve existing maps.

---

## 5. Old API Removal

The following functions are removed from the Lua API (no backwards
compatibility wrappers; no external mods use them):

- `core.terminal_set_cell`
- `core.terminal_clear`
- `core.terminal_get_size`
- `core.terminal_scan_formspec`
- `core.terminal_destroy`
- `core.send_terminal_data`
- `core.register_on_terminal_key`

Mods that used these must migrate to `ScreenBuffer` methods. The
`mods/terminal_memory/init.lua` demo is updated as the reference
implementation.

---

## 6. Persistence Strategy

Every `ScreenBuffer` is per-node. Persistence is the mod's
responsibility, with two natural storage locations:

- **Node meta** — survives server restart, lives with the node.
  This is the natural choice for buffer state.
- **`mod_storage`** — global, keyed by node hash. This is useful for
  backups or analytics, but redundant for normal operation.

### Recommended Pattern

```lua
on_construct = function(pos)
    local meta = minetest.get_meta(pos)
    local buf = ScreenBuffer(pos, "scr")
    local saved = meta:get_string("buffer")
    if saved and saved ~= "" then
        buf:deserialize(saved)
    else
        buf:clear()
    end
end

on_destruct = function(pos)
    local buf = ScreenBuffer(pos, "scr")
    minetest.get_meta(pos):set_string("buffer", buf:serialize())
end
```

The engine does **not** auto-serialize the buffer. The mod is
explicit about when to save (e.g. on `on_destruct`, on form close,
on idle).

For LBM-based restoration on map load, the mod calls
`buf:deserialize` after `on_construct` or in a registered LBM.

---

## 7. NanoBasic Integration (Phase 4)

This section is **forward-looking**. NanoBasic (`nb_lua.c`, the
`techage:basic_terminal` mod) is already published and used in
techage. Changes to NanoBasic happen only when:

1. The Luanti PR for `ScreenBuffer` is merged and shipped, AND
2. Players explicitly request the migration, AND
3. The migration preserves a runtime fallback so that old Luanti
   clients can still talk to a server running new NanoBasic.

### Goals

- Replace NanoBasic's in-process `screen_buffer[20*61]` with a Luanti
  `ScreenBuffer`, so output goes directly to the player's screen
  element via DIFF rather than through a re-rendered Formspec string.
- Keep the BASIC REPL behavior (`PRINT`, `CLS`, `LOCATE`) but route
  the output through VT100 escape sequences that `buf:write`
  understands.
- Add first-class keyboard input (so `INPUT` works in BASIC, not just
  through custom input forms).

### Non-Goals

- No removal of the existing `nanobasic.get_screen_buffer` /
  `nanobasic.set_output_callback` API in the first migration. They
  remain for backwards compatibility with existing mods.
- No breaking changes to the BASIC language itself. The VM is
  unchanged; only the I/O layer is rewritten.

### C-Lib Changes (`nb_lua.c`)

The current `nb_cpu_t` has:

```c
typedef struct {
    void *pv_vm;
    char *p_src;
    int src_pos;
    char screen_buffer[MAX_LINES * MAX_LINE_LEN];  // 20*61 = 1220 bytes
    uint8_t xpos;
    uint8_t ypos;
    lua_State *L_cb;
    int output_ref;
} nb_cpu_t;
```

The new `nb_cpu_t` drops the screen-buffer-related fields:

```c
typedef struct {
    void *pv_vm;
    char *p_src;
    int src_pos;
    lua_State *L_cb;
    int output_ref;
} nb_cpu_t;
```

This is a `pack_vm`/`unpack_vm` **breaking change**: the serialized
size changes (by exactly `MAX_LINES * MAX_LINE_LEN + 2` bytes for the
buffer plus xpos/ypos). The migration path is:

- A new magic byte or version field at the start of the packed
  payload. The C code detects the old format by size and refuses
  to load it, prompting the mod to recompile.
- A mod-side `nanobasic.vm_restore(pos)` is updated to handle the
  fallback: if the stored VM is in the old format, the mod re-asks
  the player to re-compile the program (this is rare; mods that
  store BASIC programs are typically the player's own).

### New `nb_print` Behavior

`nb_print` no longer writes to a local buffer. It always calls the
Lua callback (if registered):

```c
void nb_print(const char * format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    call_output_cb(p_Cpu, buffer, strlen(buffer));
}
```

This is a single atomic callback. The bulk-write optimization (one
callback per `nb_print` call rather than per character) is the
primary performance improvement.

### VM Opcodes as VT100 Sequences

`SETCUR`, `CLRSCR`, `CLRLINE` no longer touch an in-process buffer.
They emit ANSI escape sequences:

```c
case SETCUR: {
    y = nb_pop_num(C->pv_vm);
    x = nb_pop_num(C->pv_vm);
    char seq[16];
    int len = snprintf(seq, sizeof(seq), "\033[%d;%dH", y, x);
    call_output_cb(C, seq, len);
    break;
}
case CLRSCR:
    call_output_cb(C, "\033[2J\033[H", 7);
    break;
case CLRLINE: {
    y = nb_pop_num(C->pv_vm);
    if (y > 0) {
        char seq[24];
        int len = snprintf(seq, sizeof(seq), "\033[%d;1H\033[2K", y);
        call_output_cb(C, seq, len);
    } else {
        call_output_cb(C, "\r\033[2K", 5);
    }
    break;
}
```

This is what the C code already does in the callback branch of
the current implementation. The change is that the **non-callback
branch is removed** — the C code no longer has a fallback path that
writes to a local buffer.

### Bulk-Write API

A new `nanobasic.bulk_write(pos, text)` is added for the common case
where a mod wants to push a known string to the screen in one shot:

```c
static int bulk_write(lua_State *L) {
    nb_cpu_t *C = check_vm(L);
    if (C == NULL) return 0;
    size_t len;
    const char *text = luaL_tolstring(L, 2, &len);
    if (text != NULL && len > 0) {
        call_output_cb(C, text, len);
    }
    return 0;
}
```

This is the same code path as `nb_print` but skips the `vsnprintf`
overhead, so it's the preferred API for mods that already have a
formatted string.

### Keyboard Input

A new `nanobasic.push_key(pos, byte)` pushes one byte (or a short
byte string) into a per-VM input queue. The C VM exposes two new
helper functions for the BASIC program to read from this queue:

```c
// In nb.h:
uint8_t nb_kbhit(void *pv_vm);    // number of pending bytes
uint8_t nb_getchar(void *pv_vm);  // blocking read of one byte
```

The corresponding VM opcodes are added:

- `KBHIT` — push `nb_kbhit(pv_vm)` onto the parameter stack
- `GETCHAR` — block until a byte is available, push it

These opcodes are gated by a compile-time flag (e.g.
`-DNANOBASIC_HAS_KBHIT`) so that old compiled programs do not
reference them. The BASIC compiler is updated to emit them only when
the user explicitly requests a new I/O mode.

For old Luanti clients, the mod detects the missing opcode at
runtime and falls back to the existing `INPUT` form-field approach.
This is the backwards-compat guarantee: old clients see no change.

### Mod-Side Integration

A new mod `nanobasic_screen` (or extension of `techage:basic_terminal`)
binds the NanoBasic VM to a `ScreenBuffer`:

```lua
on_construct = function(pos)
    local meta = minetest.get_meta(pos)
    local code = meta:get_string("code") or ""
    local buf = ScreenBuffer(pos, "scr", "raw_color")
    local saved = meta:get_string("buffer")
    if saved and saved ~= "" then
        buf:deserialize(saved)
    else
        buf:clear()
    end

    if nanobasic.create(pos, code) then
        nanobasic.set_output_callback(pos, function(text)
            local sbuf = ScreenBuffer(pos, "scr")
            sbuf:write(text)
        end)
    end
end

on_timer = function(pos, elapsed)
    local res = nanobasic.run(pos, 100)
    -- Engine flushes DIFF automatically; mod does not touch the
    -- screen buffer itself in the hot path.
end

on_destruct = function(pos)
    if nanobasic.is_loaded(pos) then
        nanobasic.set_output_callback(pos, nil)
    end
    local buf = ScreenBuffer(pos, "scr")
    minetest.get_meta(pos):set_string("buffer", buf:serialize())
end

on_rightclick = function(pos, node, clicker)
    local fs = "formspec_version[12];" ..
               "size[14,8];" ..
               "screen[0.5,0.8;13,6;scr;61;20;raw_color];" ..
               "button[0.5,7;1,0.5;quit;Close]"
    core.show_formspec(clicker:get_player_name(),
        "nodemeta@" .. pos.x .. "," .. pos.y .. "," .. pos.z, fs)
end

on_receive_fields = function(pos, formname, fields, player)
    if fields.quit then
        core.close_formspec(player:get_player_name(), formname)
    end
end
```

The keyboard input (when `buf:on_key` is implemented) is added in
a later phase:

```lua
buf:on_key(function(player, byte)
    nanobasic.push_key(pos, byte)
end)
```

### Versioning

A new `nanobasic.version()` returns a version string that includes
flags:

```
"1.1.0+kbhit"
```

Mods check this to decide whether to use `on_key` (new) or the
form-field fallback (old).

---

## 8. Class LuaDoc Skeleton

For the Luanti `lua_api.md` Class Reference section:

```lua
ScreenBuffer
------------

A server-side, persistent character grid that lives with a node and is
automatically synchronized to all players viewing a formspec that
declares the matching `screen[]` element. See [Formspec elements]
(#formspec-elements) for the `screen[]` declaration.

Can be created via:

* `ScreenBuffer(pos, element_name)` — type is taken from the node's
  formspec
* `ScreenBuffer(pos, element_name, type)` — type is explicit

### Methods

* `get_pos()`: returns the node position.
* `get_element_name()`: returns the element name.
* `get_formname()`: returns `"nodemeta@<x>,<y>,<z>"`.
* `get_type()`: returns `"raw"`, `"raw_color"`, or `"vt100"`.
* `get_cols()`, `get_rows()`, `get_size()`: buffer dimensions.
* `get_version()`: DIFF counter.
* `is_valid()`: false if the node was removed.

* `set_cell(col, row, char [, fg, bg])`, `get_cell(col, row)`,
  `clear()`: low-level cell access.

* `set_cursor(col, row)`, `get_cursor()`: cursor state for the
  high-level `write_*` methods.
* `write_char(char [, fg, bg])`, `write_string(text [, fg, bg])`,
  `write_line(row, col, text [, fg, bg])`, `write(text)`: write
  characters, strings, or VT100 byte streams.

* `fill_rect(...)`, `scroll(lines)`, `draw_box(...)`: area ops.

* `serialize()`: Base64 string for storage.
* `deserialize(str)`: restore from Base64.

* `set_sync(bool)`, `flush()`, `get_dirty_count()`: sync control.

* `on_key(callback)`: register input callback (server-side).
* `has_focus()` (client), `request_focus()` (client),
  `release_focus()`, `get_focus_owner()` (server): focus management.

### Example

```lua
local buf = ScreenBuffer(pos, "scr")
buf:clear()
buf:draw_box(0, 0, 79, 23, "double")
buf:set_cursor(2, 1)
buf:write_string("Hello, World!", 7, 0)
buf:write_string("Persistent buffer demo.", 3, 0)
mod_storage:set_string("greeting", buf:serialize())
```

### Notes

* `ScreenBuffer` instances are lightweight handles. The underlying
  storage is shared across all `ScreenBuffer` objects that point to
  the same `(formname, element_name)` pair.
* For `vt100` type buffers, `set_cell` and `get_cell` still work
  (the cell at the cursor is the "current" one), but the natural
  way to drive a VT100 screen is via `buf:write(text)` with ANSI
  escape sequences.
```

---

## 9. Open Questions

1. **Backward compat alias for `terminal[]`**: how long? Suggest
   "until the next Luanti release after merge" then remove.
2. **Multi-element buffers per formname**: e.g. one terminal, one
   text label. Each `ScreenBuffer` is a single element, the mod
   holds multiple references. The engine handles each one
   independently. Already the design; no issue.
3. **Maximum cells per buffer**: 240 × 60 = 14400 cells, current cap.
   Should it stay? Reasonable; 14400 × 6 = 86 KB wire payload for
   INIT, which is OK for a 200 ms DIFF.
4. **Focus timeout default**: 60 s. Should it be configurable
   per-buffer? Yes, but as a later feature.
5. **VT100 attribute parsing (SGR)**: bold, underline, reverse, color.
   Not required for NanoBasic BASIC but useful for retro games.
   Phase 2 of the engine work.
6. **`on_key` callback execution context**: server-side, in the main
   server thread, after the keystroke packet is received. This
   means a slow callback blocks the server. Mods must keep the
   callback cheap (no heavy I/O, no infinite loops).
7. **Cursor state in `raw` / `raw_color`**: the high-level `write_*`
   methods are designed for VT100-style use. For `raw` buffers used
   as static image frames (e.g. the existing `terminal_memory` demo),
   the mod still uses `set_cell` directly. The cursor state is
   irrelevant for those use cases.
8. **Per-peer visibility of the focus owner**: the form must include
   the focus-owner's name. A future API
   `buf:get_focus_owner_display()` returns a label like
   `"Read-only — bob is typing"`, the mod embeds it as a `label[]`
   element in the formspec.

---

## 10. Implementation Order

| Step | Component | Effort | Notes |
|---|---|---|---|
| 1 | Engine: `ScreenBuffer` class with cell access, `set_cursor`, `get_cursor`, `clear`, `write_char`, `write_string`, `write_line` | M | rename `l_terminal*` → `l_screenbuffer*` |
| 2 | Engine: `fill_rect`, `scroll`, `draw_box` | S | basic math, no protocol change |
| 3 | Engine: `serialize`/`deserialize` (Base64) | S | use `core.encode_base64` |
| 4 | Engine: rename Formspec element `terminal[...]` → `screen[...]`, keep alias | S | parser change |
| 5 | Engine: `buf:write(text)` VT100 parser | M | CUP, ED, EL, LF, CR, TAB |
| 6 | Engine: per-buffer input queue + focus tracking | M | new state in `ServerScreenBuffer` |
| 7 | Engine: wire opcodes `TOCLIENT_SCREEN_FOCUS` (0x68), `TOSERVER_SCREEN_INPUT` (0x68) | M | protocol bump |
| 8 | Engine: `buf:on_key(callback)` server-side | M | Lua-level method, callback dispatch |
| 9 | Engine: client-side focus request + UI indicator | M | guiScreen changes |
| 10 | Engine: remove old `core.terminal_*` API | S | breaking, but no external mods |
| 11 | Engine: SGR attribute parsing | M | bold, reverse, 8-color fg/bg |
| 12 | Mod: migrate `terminal_memory` to `ScreenBuffer` API | S | reference implementation |
| 13 | Mod: migrate `techage:basic_terminal` to new screen element (optional) | M | backwards-compat fallback path |
| 14 | C: NanoBasic `nb_lua.c` refactor (drop `screen_buffer`) | M | requires C-level coordination |
| 15 | C: `nb_push_key`, `nb_kbhit`, `nb_getchar` | M | new opcodes, gated by macro |
| 16 | C: `bulk_write` API | S | optimization over `nb_print` |
| 17 | C: `pack_vm` version field + size change | S | breaking, but no migration path needed |
| 18 | Mod: `nanobasic_screen` integration mod | M | ties C and Lua together |
| 19 | Docs: lua_api.md, screen_buffer.md (this file) | S | |

Effort: S = < 1 day, M = 1-3 days.

Total: roughly 4-6 weeks for a single developer, end-to-end.

---

## 11. Risk Register

| Risk | Mitigation |
|---|---|
| `pack_vm` size change breaks existing saves | Add a magic/version field at the start, refuse to load old data, mod re-asks player to recompile |
| Slow `on_key` callback blocks server | Document the warning prominently; the existing Lua sandbox is already a soft guarantee |
| Focus race condition when two peers click simultaneously | Server arbitrates, second client gets denied |
| `screen_buffer` removal in C breaks other uses | Audit `nb_lua.c` and `nb_lua.h`; the buffer is local to `nb_cpu_t`, no other consumer |
| `screen[...]` Formspec parser change breaks existing maps | Keep `terminal[...]` alias for one Luanti release |
| Multi-element formname race | Each element is independent in `m_terminal_buffers`; no cross-talk |
| VT100 parser bugs (e.g. malformed escapes) | Robust error recovery: skip to next printable byte, log warning |
| `serialize` size exceeds `TERMINAL_MAX_DATA_LEN` (64 KiB) | Cap remains; current dimensions (240 × 60) fit comfortably |
| Cursor state across Server restart (C refactor) | Mod serializes cursor as part of `buf:deserialize`; same as cell state |
| Cell aspect ratio depends on font | Luanti's `GUITerminal::draw()` reads `cell_w = char_dim.Width` and `cell_h = char_dim.Height + 2` from the font's "M" advance. With a monospace font whose glyphs are taller than wide (e.g. Luanti's default `Cousine-Regular.ttf`), cells render as ~1:2. The user must select a font with **square or near-square glyph metrics** (e.g. 8×8 pixel or 7×9 pixel) to get 1:1 cells. The formspec sizing code cannot fix this — only a different font can. |

---

## 12. Acceptance Criteria

A PR is ready for review when:

1. All old `core.terminal_*` functions are removed; no wrappers.
2. `ScreenBuffer` class is implemented with all methods listed in
   section 3.
3. `screen[...]` Formspec element parses; `terminal[...]` works as
   alias.
4. The existing `mods/terminal_memory` demo runs without
   modification to its formspec syntax, but with the API call
   surface changed to `ScreenBuffer` methods.
5. Multi-client test: two clients open the same node's form, both
   see the same screen content (DIFF replicated), only one has
   input focus.
6. Persistence test: place a node, write to its screen, restart
   the server, reopen the form — the screen content is preserved
   from `serialize`/`deserialize`.
7. Build is clean, no warnings, no debug log spam.
8. `STATUS.md` is updated to reflect the final design.
9. `lua_api.md` is updated with the new class documentation.

The NanoBasic integration (steps 14-18) is **not** part of the
upstream PR. It lives in the user's separate NanoBasic repo and is
merged only after the upstream PR is accepted and players request
it.
