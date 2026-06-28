// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <deque>

/*
 * Server-side terminal buffer model.
 *
 * Used for terminal[] formspec elements with type="raw" or type="raw_color".
 * The canonical cell grid is stored on the server. Mods manipulate the
 * buffer through Lua (core.terminal_set_cell etc.) and the server is
 * responsible for streaming the current state to attached clients.
 *
 * Lifetime:
 *  - create() is called the first time a raw / raw_color terminal element
 *    is parsed on a show_formspec. The buffer lives until the formspec is
 *    closed (any further show_formspec with the same formname updates dims
 *    in-place; the cells are preserved if cols*rows doesn't change).
 *  - destroy() is called when the formspec is closed (quit=true or empty
 *    show_formspec).
 *  - clear() resets the cell grid (memory remains allocated).
 *
 * Versioning:
 *  - current_version starts at 0. Every call to setCell / clear / resize
 *    bumps it.
 *  - For each peer we record the last version the peer has acknowledged.
 *    A peer at version 0 is "new" and will get a TOCLIENT_TERMINAL_INIT
 *    (full grid). A peer behind current_version gets a TOCLIENT_TERMINAL_DIFF
 *    with only the cells that changed since its last version.
 */

enum TerminalType : u8 {
	TERMINAL_TYPE_VT100      = 0,
	TERMINAL_TYPE_RAW        = 1,
	TERMINAL_TYPE_RAW_COLOR  = 2,
};

struct ServerTerminalCell {
	wchar_t ch = L' ';  // glyph codepoint
	u8 fg = 7;          // TERM_COLOR_WHITE
	u8 bg = 0;          // TERM_COLOR_BLACK
};

class ServerTerminalBuffer {
public:
	ServerTerminalBuffer() = default;
	ServerTerminalBuffer(TerminalType type, u16 cols, u16 rows);

	TerminalType getType() const { return m_type; }
	u16 getCols() const { return m_cols; }
	u16 getRows() const { return m_rows; }
	u32 getVersion() const { return m_current_version; }
	u32 getCellCount() const { return m_cols * m_rows; }
	// Size in bytes of one cell in wire format.
	u8 getCellWireSize() const;

	const std::vector<ServerTerminalCell> &getCells() const { return m_cells; }
	const ServerTerminalCell &cell(u16 col, u16 row) const
	{
		return m_cells[row * m_cols + col];
	}

	// Resize the buffer. Bumps the version. Preserves existing cells
	// where possible; new cells are filled with the default (space,
	// white-on-black).
	void resize(u16 cols, u16 rows);

	// Set a single cell. fg/bg are ignored for TERMINAL_TYPE_RAW. Bumps
	// the version only if the cell actually changed.
	void setCell(u16 col, u16 row, wchar_t ch, u8 fg = 7, u8 bg = 0);

	// Reset all cells to default. Bumps the version.
	void clear();

	// Cursor state (used by the high-level write methods).
	void setCursor(u16 x, u16 y) { m_cursor_x = x; m_cursor_y = y; }
	u16 getCursorX() const { return m_cursor_x; }
	u16 getCursorY() const { return m_cursor_y; }
	// Write a cell at the current cursor position, then advance the
	// cursor by one column (wrapping to the next row at the right
	// edge). fg/bg apply to the current cell.
	void setCursorCell(wchar_t ch, u8 fg = 7, u8 bg = 0);

	// Serialize the entire grid into a contiguous byte buffer. Used for
	// TOCLIENT_TERMINAL_INIT.
	void serializeAll(std::string &out) const;

	// Append the cells that changed since `since_version` to `out`, in
	// the wire format documented on TOCLIENT_TERMINAL_DIFF:
	//   u16 cell_index (row-major)
	//   u8  cell_size
	//   u8[cell_size] cell_bytes
	// Returns the number of cells written. A return value of 0 means
	// "no changes since since_version" (caller should skip the packet).
	u32 serializeChangedSince(u32 since_version, std::string &out) const;

private:
	void bumpVersion() { m_current_version++; }

	TerminalType m_type = TERMINAL_TYPE_RAW;
	u16 m_cols = 0;
	u16 m_rows = 0;
	u32 m_current_version = 0;
	std::vector<ServerTerminalCell> m_cells;
	// Cells that changed in the most recent setCell / clear / resize
	// call. We track this here so that the periodic flush can emit
	// a single DIFF packet per buffer covering exactly those cells,
	// without having to walk the whole grid comparing against each
	// peer's last-seen version.
	std::vector<u16> m_dirty_indices;
	// Cursor position for high-level write methods. 0-indexed.
	// For "vt100" buffers this is also the visual cursor; for "raw"/
	// "raw_color" buffers it is used by buf:write_char etc.
	u16 m_cursor_x = 0;
	u16 m_cursor_y = 0;
	// Current fg/bg colors for high-level write methods.
	u8 m_cur_fg = 7;
	u8 m_cur_bg = 0;
	// Per-buffer input queue (FIFO). Filled by client keystroke
	// packets; drained by the per-buffer on_key Lua callback.
	std::deque<u8> m_input_queue;
	// Player who currently holds input focus (server-side state).
	// Empty string means "no one has focus".
	std::string m_focus_owner;
	// Timestamp (ms since server start) at which focus was taken;
	// used for idle timeout.
	u32 m_focus_taken_at_ms = 0;
};

/*
 * Storage for all currently-live ServerTerminalBuffers, keyed by
 * "formname\0element_name" (a control char that cannot appear in
 * valid form/element names per the formspec grammar).
 *
 * Per-peer last-seen version is tracked in a parallel map so the
 * flush loop can decide between INIT (peer_version == 0) and DIFF.
 */
class ServerTerminalStore {
public:
	// Find an existing buffer; returns nullptr if not present.
	ServerTerminalBuffer *find(const std::string &formname,
		const std::string &element_name);
	const ServerTerminalBuffer *find(const std::string &formname,
		const std::string &element_name) const;

	// Get-or-create. If the buffer exists but dims differ, it is resized
	// in-place (which preserves any pre-existing cell data for the
	// overlapping region).
	ServerTerminalBuffer &getOrCreate(TerminalType type, u16 cols, u16 rows,
		const std::string &formname, const std::string &element_name);

	// Destroy the buffer for a given formspec + element. Returns the
	// number of buffers actually removed.
	size_t destroy(const std::string &formname, const std::string &element_name);

	// Destroy every buffer that belongs to `formname`. Used when the
	// formspec is closed. Returns the number of buffers removed.
	size_t destroyAllForFormspec(const std::string &formname);

	// Get / set the per-peer last-seen version. Returns 0 if the peer
	// has never seen this buffer.
	u32 getPeerVersion(const std::string &formname,
		const std::string &element_name, u16 peer_id) const;
	void setPeerVersion(const std::string &formname,
		const std::string &element_name, u16 peer_id, u32 version);

	// Forget every per-peer version. Called when the buffer itself
	// is destroyed or recreated, to ensure the next attach forces
	// a fresh INIT.
	void resetPeerVersions(const std::string &formname,
		const std::string &element_name);

	// Drop every (buffer, peer) version entry that mentions `peer_id`.
	// Called from Server::DeleteClient to keep the per-peer map from
	// leaking disconnected clients.
	void forgetPeer(u16 peer_id);

	// Iterate every live buffer. Used by the 200ms flush loop.
	template <typename F>
	void forEachBuffer(F &&fn)
	{
		for (auto &kv : m_buffers)
			fn(kv.first, kv.second);
	}

	// Split a "formname\x1felement_name" key back into its parts.
	// Returns false if the key doesn't have the expected shape.
	static bool splitKey(const std::string &key,
		std::string &formname, std::string &element_name);

private:
	static std::string makeKey(const std::string &formname,
		const std::string &element_name);

	std::unordered_map<std::string, ServerTerminalBuffer> m_buffers;
	// key = formname\0element_name, value = peer_id -> last_version
	std::unordered_map<std::string, std::unordered_map<u16, u32>> m_peer_versions;
};
