// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "serverterminal.h"
#include "log.h"
#include "network/networkprotocol.h" // TERMINAL_MAX_DATA_LEN
#include "util/serialize.h" // writeU16 / writeU32

ServerTerminalBuffer::ServerTerminalBuffer(TerminalType type, u16 cols, u16 rows) :
	m_type(type), m_cols(cols), m_rows(rows)
{
	m_cells.assign(static_cast<size_t>(cols) * rows, ServerTerminalCell{});
	m_current_version = 1; // start at 1 so a fresh peer (version 0) is recognised
}

u8 ServerTerminalBuffer::getCellWireSize() const
{
	switch (m_type) {
	case TERMINAL_TYPE_RAW:       return 4; // u32 codepoint
	case TERMINAL_TYPE_RAW_COLOR: return 6; // u32 + u8 + u8
	case TERMINAL_TYPE_VT100:
	default:                      return 0; // not used for VT100
	}
}

void ServerTerminalBuffer::resize(u16 cols, u16 rows)
{
	if (cols == m_cols && rows == m_rows)
		return;
	std::vector<ServerTerminalCell> new_cells(static_cast<size_t>(cols) * rows,
		ServerTerminalCell{});
	// Preserve overlapping cells.
	u16 copy_cols = std::min(cols, m_cols);
	u16 copy_rows = std::min(rows, m_rows);
	for (u16 r = 0; r < copy_rows; r++) {
		for (u16 c = 0; c < copy_cols; c++) {
			new_cells[r * cols + c] = m_cells[r * m_cols + c];
		}
	}
	m_cells = std::move(new_cells);
	m_cols = cols;
	m_rows = rows;
	m_dirty_indices.clear();
	bumpVersion();
}

void ServerTerminalBuffer::setCell(u16 col, u16 row, wchar_t ch, u8 fg, u8 bg)
{
	if (col >= m_cols || row >= m_rows)
		return;
	u16 idx = row * m_cols + col;
	auto &c = m_cells[idx];
	bool changed = (c.ch != ch) || (c.fg != fg) || (c.bg != bg);
	if (!changed)
		return;
	c.ch = ch;
	c.fg = fg;
	c.bg = bg;
	m_dirty_indices.push_back(idx);
	bumpVersion();
}

void ServerTerminalBuffer::clear()
{
	for (auto &c : m_cells) {
		if (c.ch == L' ' && c.fg == 7 && c.bg == 0)
			continue;
	}
	// Just reset; we'll mark everything dirty in a moment.
	std::fill(m_cells.begin(), m_cells.end(), ServerTerminalCell{});
	m_dirty_indices.clear();
	m_dirty_indices.reserve(m_cells.size());
	for (u16 i = 0; i < m_cells.size(); i++)
		m_dirty_indices.push_back(i);
	bumpVersion();
}

void ServerTerminalBuffer::serializeAll(std::string &out) const
{
	u8 cell_size = getCellWireSize();
	out.reserve(out.size() + m_cells.size() * cell_size);
	u8 buf[6];
	for (const auto &c : m_cells) {
		u32 cp = static_cast<u32>(c.ch);
		writeU32(buf, cp);
		out.append(reinterpret_cast<const char*>(buf), 4);
		if (m_type == TERMINAL_TYPE_RAW_COLOR) {
			buf[0] = c.fg;
			buf[1] = c.bg;
			out.append(reinterpret_cast<const char*>(buf), 2);
		}
	}
}

u32 ServerTerminalBuffer::serializeChangedSince(u32 since_version, std::string &out) const
{
	// For a "first attach" (since_version == 0) we have no prior state to
	// diff against on the wire, so the caller should use serializeAll()
	// via a TOCLIENT_TERMINAL_INIT packet.
	// Here we just emit the cells tracked in m_dirty_indices.
	// If the buffer was resized since since_version we also need to emit
	// the cells in the previously-uncovered area; for simplicity we always
	// treat resize as a full re-init from the perspective of the diff
	// path -- the flush loop will detect a dimension change and fall
	// back to INIT for affected peers.
	if (since_version == 0)
		return 0;
	// Deduplicate dirty indices while preserving order.
	// For a typical workload (a small handful of cells per tick) a linear
	// scan is faster than building a set.
	std::vector<u16> uniq;
	uniq.reserve(m_dirty_indices.size());
	for (u16 idx : m_dirty_indices) {
		bool seen = false;
		for (u16 u : uniq) {
			if (u == idx) { seen = true; break; }
		}
		if (!seen)
			uniq.push_back(idx);
	}
	u8 cell_size = getCellWireSize();
	u8 buf[6];
	for (u16 idx : uniq) {
		const auto &c = m_cells[idx];
		writeU16(buf, idx);
		out.push_back(static_cast<char>(buf[0]));
		out.push_back(static_cast<char>(buf[1]));
		out.push_back(static_cast<char>(cell_size));
		u32 cp = static_cast<u32>(c.ch);
		writeU32(buf, cp);
		out.append(reinterpret_cast<const char*>(buf), 4);
		if (m_type == TERMINAL_TYPE_RAW_COLOR) {
			out.push_back(static_cast<char>(c.fg));
			out.push_back(static_cast<char>(c.bg));
		}
	}
	return uniq.size();
}

// ---- ServerTerminalStore ----

std::string ServerTerminalStore::makeKey(const std::string &formname,
	const std::string &element_name)
{
	// 0x1f is the ASCII "Unit Separator" control character; formspec names
	// cannot contain it (the parser splits on '['/';'/',' which are all
	// printable), so it's a safe delimiter.
	return formname + "\x1f" + element_name;
}

ServerTerminalBuffer *ServerTerminalStore::find(const std::string &formname,
	const std::string &element_name)
{
	auto it = m_buffers.find(makeKey(formname, element_name));
	return it != m_buffers.end() ? &it->second : nullptr;
}

const ServerTerminalBuffer *ServerTerminalStore::find(const std::string &formname,
	const std::string &element_name) const
{
	auto it = m_buffers.find(makeKey(formname, element_name));
	return it != m_buffers.end() ? &it->second : nullptr;
}

ServerTerminalBuffer &ServerTerminalStore::getOrCreate(TerminalType type,
	u16 cols, u16 rows, const std::string &formname,
	const std::string &element_name)
{
	std::string key = makeKey(formname, element_name);
	auto it = m_buffers.find(key);
	if (it == m_buffers.end()) {
		auto inserted = m_buffers.emplace(std::piecewise_construct,
			std::forward_as_tuple(key),
			std::forward_as_tuple(type, cols, rows));
		// Any peer that was watching a previous buffer under the same key
		// is now stale: force a fresh INIT by clearing their version.
		resetPeerVersions(formname, element_name);
		return inserted.first->second;
	}
	auto &buf = it->second;
	if (buf.getType() != type || buf.getCols() != cols || buf.getRows() != rows) {
		// Type mismatch is a mod bug; we keep the existing type but adjust
		// dims. The mismatch is logged once for the modder.
		if (buf.getType() != type) {
			warningstream << "ServerTerminalStore: type mismatch for "
				<< formname << "/" << element_name
				<< " (existing=" << (int)buf.getType()
				<< " new=" << (int)type
				<< "); keeping existing type" << std::endl;
		}
		buf.resize(cols, rows);
		// Force re-init on the next attach, since the cells may have been
		// partially re-mapped.
		resetPeerVersions(formname, element_name);
	}
	return buf;
}

size_t ServerTerminalStore::destroy(const std::string &formname,
	const std::string &element_name)
{
	std::string key = makeKey(formname, element_name);
	m_peer_versions.erase(key);
	return m_buffers.erase(key);
}

size_t ServerTerminalStore::destroyAllForFormspec(const std::string &formname)
{
	size_t n = 0;
	std::string prefix = formname + "\x1f";
	for (auto it = m_buffers.begin(); it != m_buffers.end(); ) {
		if (it->first.compare(0, prefix.size(), prefix) == 0) {
			m_peer_versions.erase(it->first);
			it = m_buffers.erase(it);
			n++;
		} else {
			++it;
		}
	}
	return n;
}

u32 ServerTerminalStore::getPeerVersion(const std::string &formname,
	const std::string &element_name, u16 peer_id) const
{
	auto kit = m_peer_versions.find(makeKey(formname, element_name));
	if (kit == m_peer_versions.end())
		return 0;
	auto vit = kit->second.find(peer_id);
	return vit != kit->second.end() ? vit->second : 0;
}

void ServerTerminalStore::setPeerVersion(const std::string &formname,
	const std::string &element_name, u16 peer_id, u32 version)
{
	auto &map = m_peer_versions[makeKey(formname, element_name)];
	map[peer_id] = version;
}

void ServerTerminalStore::resetPeerVersions(const std::string &formname,
	const std::string &element_name)
{
	m_peer_versions.erase(makeKey(formname, element_name));
}

void ServerTerminalStore::forgetPeer(u16 peer_id)
{
	for (auto &kv : m_peer_versions) {
		kv.second.erase(peer_id);
	}
}

bool ServerTerminalStore::splitKey(const std::string &key,
	std::string &formname, std::string &element_name)
{
	auto pos = key.find('\x1f');
	if (pos == std::string::npos)
		return false;
	formname = key.substr(0, pos);
	element_name = key.substr(pos + 1);
	return true;
}
