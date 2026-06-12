// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2024 Luanti contributors

#include "guiTerminal.h"

#include <IGUIEnvironment.h>
#include <IGUISkin.h>
#include <IGUIFont.h>
#include <IVideoDriver.h>
#include <IEventReceiver.h>
#include <Keycodes.h>
#include <algorithm>
#include <cstring>
#include "client/fontengine.h"
#include "util/serialize.h"
#include "log.h"

// Standard VT100/ANSI 8-color palette (dark variants)
const video::SColor GUITerminal::s_palette[8] = {
	video::SColor(255,   0,   0,   0), // black
	video::SColor(255, 170,   0,   0), // red
	video::SColor(255,   0, 170,   0), // green
	video::SColor(255, 170, 170,   0), // yellow
	video::SColor(255,   0,   0, 170), // blue
	video::SColor(255, 170,   0, 170), // magenta
	video::SColor(255,   0, 170, 170), // cyan
	video::SColor(255, 170, 170, 170), // white
};

GUITerminal::GUITerminal(gui::IGUIEnvironment *env, gui::IGUIElement *parent,
	s32 id, core::rect<s32> rect, u32 cols, u32 rows)
	: gui::IGUIElement(gui::EGUIET_ELEMENT, env, parent, id, rect)
	, m_cols(std::min(cols, (u32)TERMINAL_MAX_COLS))
	, m_rows(std::min(rows, (u32)TERMINAL_MAX_ROWS))
	, m_cells(m_cols * m_rows)
{
}

GUITerminal::~GUITerminal() = default;

void GUITerminal::setOverrideFont(gui::IGUIFont *font)
{
	m_font = font;
}

void GUITerminal::setFontSizeOverride(u32 font_size)
{
	m_font_size_override = font_size;
	m_font = nullptr; // force draw() to look up a new font on next frame
}

GUITerminal::TerminalState GUITerminal::saveState() const
{
	return { m_cells, m_cur_col, m_cur_row, m_cur_attr, m_font_size_override };
}

void GUITerminal::restoreState(const TerminalState &s)
{
	// Restore cells up to the current grid size
	u32 n = std::min((u32)s.cells.size(), m_cols * m_rows);
	std::copy(s.cells.begin(), s.cells.begin() + n, m_cells.begin());
	m_cur_col  = std::min(s.cur_col, m_cols - 1);
	m_cur_row  = std::min(s.cur_row, m_rows - 1);
	m_cur_attr = s.cur_attr;
	// Font size survives formspec regeneration so the +/- zoom buttons
	// behave like a persistent preference within a session.
	m_font_size_override = s.font_size;
	if (m_font_size_override != 0)
		m_font = nullptr; // force re-lookup at the new size
}

void GUITerminal::reset()
{
	std::fill(m_cells.begin(), m_cells.end(), TermCell{});
	m_cur_col = 0;
	m_cur_row = 0;
	m_cur_attr = TermAttr{};
	m_parse_state = ParseState::NORMAL;
	m_csi_params.clear();
}

void GUITerminal::initFromServer(u8 type, u16 cols, u16 rows,
	const std::string &cell_data)
{
	// Only "raw" and "raw_color" cells are server-driven. For VT100 we
	// keep the client-side state and ignore the packet -- it might be a
	// stale init that arrived after the form was closed.
	if (type == 0)
		return;
	// Resize the grid if the server says so. This will only ever shrink
	// or expand; the client clamps to TERMINAL_MAX_COLS / ROWS in the
	// constructor, so a server that asks for more gets capped here too.
	u32 want_cols = std::min((u32)cols, (u32)TERMINAL_MAX_COLS);
	u32 want_rows = std::min((u32)rows, (u32)TERMINAL_MAX_ROWS);
	if (want_cols != m_cols || want_rows != m_rows) {
		m_cols = want_cols;
		m_rows = want_rows;
		m_cells.assign(m_cols * m_rows, TermCell{});
	} else {
		std::fill(m_cells.begin(), m_cells.end(), TermCell{});
	}
	u8 cell_size = (type == 2) ? 6 : 4; // raw_color: 6, raw: 4
	u32 cell_count = m_cols * m_rows;
	if (cell_data.size() < cell_count * cell_size) {
		// Truncated; keep what we got and zero the rest.
		infostream << "GUITerminal::initFromServer: cell_data size "
			<< cell_data.size() << " < expected " << cell_count * cell_size
			<< " for " << cell_count << " cells of " << (int)cell_size
			<< " bytes" << std::endl;
		cell_count = cell_data.size() / cell_size;
	}
	for (u32 i = 0; i < cell_count; i++) {
		const u8 *p = reinterpret_cast<const u8*>(
			cell_data.data() + i * cell_size);
		// Cell wire format: u32 codepoint (big-endian, written via
		// writeU32 in the server) followed by fg/bg bytes for raw_color.
		u32 cp = readU32(p);
		auto &c = m_cells[i];
		c.ch = (wchar_t)cp;
		c.attr = TermAttr{};
		if (type == 2) {
			c.attr.fg = p[4];
			c.attr.bg = p[5];
		}
	}
	// Cursor is meaningless for raw buffers; pin it to home.
	m_cur_col = 0;
	m_cur_row = 0;
	m_cur_attr = TermAttr{};
	// Cancel any in-flight VT100 parse state.
	m_parse_state = ParseState::NORMAL;
	m_csi_params.clear();
}

void GUITerminal::applyServerDiff(const std::string &cell_data)
{
	// Wire format: repeated (u16 idx, u8 cell_size, cell_bytes[cell_size])
	u32 i = 0;
	while (i + 3 <= cell_data.size()) {
		const u8 *p = reinterpret_cast<const u8*>(cell_data.data() + i);
		u16 idx = readU16(p);
		u8  cell_size = p[2];
		i += 3;
		if (i + cell_size > cell_data.size())
			break;
		if (idx >= m_cells.size() || cell_size < 4 || cell_size > 6) {
			i += cell_size;
			continue;
		}
		auto &c = m_cells[idx];
		const u8 *q = reinterpret_cast<const u8*>(cell_data.data() + i);
		u32 cp = readU32(q);
		c.ch = (wchar_t)cp;
		if (cell_size >= 6) {
			c.attr.fg = q[4];
			c.attr.bg = q[5];
		} else {
			c.attr = TermAttr{};
		}
		i += cell_size;
	}
}

// ---------------------------------------------------------------------------
// VT100 parser
// ---------------------------------------------------------------------------

void GUITerminal::feed(const std::string &data)
{
	for (unsigned char c : data) {
		switch (m_parse_state) {

		case ParseState::NORMAL:
			if (c == 0x1B) {           // ESC
				m_parse_state = ParseState::ESC;
			} else if (c == '\r') {
				m_cur_col = 0;
			} else if (c == '\n') {
				m_cur_col = 0;
				if (m_cur_row + 1 < m_rows)
					++m_cur_row;
				else
					scrollUp();
			} else if (c == '\b') {
				if (m_cur_col > 0) --m_cur_col;
			} else if (c == '\t') {
				u32 next = (m_cur_col + 8) & ~7u;
				m_cur_col = std::min(next, m_cols - 1);
			} else if (c >= 0x20 && c < 0x7F) {
				// Printable ASCII – write at cursor
				if (m_cur_col < m_cols && m_cur_row < m_rows) {
					auto &cl = cell(m_cur_col, m_cur_row);
					cl.ch   = (wchar_t)c;
					cl.attr = m_cur_attr;
				}
				++m_cur_col;
				if (m_cur_col >= m_cols) {
					m_cur_col = 0;
					if (m_cur_row + 1 < m_rows)
						++m_cur_row;
					else
						scrollUp();
				}
			}
			// ignore other control chars (bell, etc.)
			break;

		case ParseState::ESC:
			if (c == '[') {
				m_parse_state = ParseState::CSI;
				m_csi_params.clear();
			} else if (c == 'c') {
				// RIS – full reset
				reset();
			} else {
				// Unknown ESC sequence – ignore
				m_parse_state = ParseState::NORMAL;
			}
			break;

		case ParseState::CSI:
		case ParseState::CSI_PARAM:
			if ((c >= '0' && c <= '9') || c == ';') {
				m_csi_params += (char)c;
				m_parse_state = ParseState::CSI_PARAM;
			} else {
				// Final byte of CSI sequence
				m_csi_params += (char)c; // store command char last
				processCsi();
				m_parse_state = ParseState::NORMAL;
				m_csi_params.clear();
			}
			break;
		}
	}
}

// Parse collected CSI parameters and execute the command.
// m_csi_params ends with the command character.
void GUITerminal::processCsi()
{
	if (m_csi_params.empty()) return;

	char cmd = m_csi_params.back();
	std::string params = m_csi_params.substr(0, m_csi_params.size() - 1);

	// Split params by ';'
	auto getParam = [&](int idx, int def) -> int {
		int i = 0, cur = 0;
		bool found = false;
		for (size_t p = 0; p <= params.size(); ++p) {
			if (p == params.size() || params[p] == ';') {
				if (i == idx) {
					return found ? cur : def;
				}
				++i; cur = 0; found = false;
			} else if (params[p] >= '0' && params[p] <= '9') {
				cur = cur * 10 + (params[p] - '0');
				found = true;
			}
		}
		return def;
	};

	switch (cmd) {
	case 'A': { // Cursor Up
		int n = getParam(0, 1);
		m_cur_row = (u32)std::max(0, (int)m_cur_row - n);
		break;
	}
	case 'B': { // Cursor Down
		int n = getParam(0, 1);
		m_cur_row = std::min(m_cur_row + (u32)n, m_rows - 1);
		break;
	}
	case 'C': { // Cursor Forward
		int n = getParam(0, 1);
		m_cur_col = std::min(m_cur_col + (u32)n, m_cols - 1);
		break;
	}
	case 'D': { // Cursor Back
		int n = getParam(0, 1);
		m_cur_col = (u32)std::max(0, (int)m_cur_col - n);
		break;
	}
	case 'H': // Cursor Position  ESC[row;colH  (1-based)
	case 'f': {
		int row = getParam(0, 1) - 1;
		int col = getParam(1, 1) - 1;
		m_cur_row = (u32)std::max(0, std::min(row, (int)m_rows - 1));
		m_cur_col = (u32)std::max(0, std::min(col, (int)m_cols - 1));
		break;
	}
	case 'J': { // Erase in Display
		int n = getParam(0, 0);
		if (n == 0) {
			// cursor to end
			clearRegion(m_cur_col, m_cur_row, m_cols - 1, m_cur_row);
			if (m_cur_row + 1 < m_rows)
				clearRegion(0, m_cur_row + 1, m_cols - 1, m_rows - 1);
		} else if (n == 1) {
			// start to cursor
			if (m_cur_row > 0)
				clearRegion(0, 0, m_cols - 1, m_cur_row - 1);
			clearRegion(0, m_cur_row, m_cur_col, m_cur_row);
		} else if (n == 2) {
			// whole screen
			clearRegion(0, 0, m_cols - 1, m_rows - 1);
			m_cur_col = 0;
			m_cur_row = 0;
		}
		break;
	}
	case 'K': { // Erase in Line
		int n = getParam(0, 0);
		if (n == 0)
			clearRegion(m_cur_col, m_cur_row, m_cols - 1, m_cur_row);
		else if (n == 1)
			clearRegion(0, m_cur_row, m_cur_col, m_cur_row);
		else if (n == 2)
			clearRegion(0, m_cur_row, m_cols - 1, m_cur_row);
		break;
	}
	case 'm': { // Select Graphic Rendition (attributes)
		if (params.empty()) {
			applyAttrCode(0);
		} else {
			// May have multiple codes separated by ';'
			int i = 0;
			while (true) {
				int code = getParam(i++, -1);
				if (code < 0) break;
				applyAttrCode(code);
				// Check if there are more by seeing if index i would be valid
				// (getParam returns def=-1 if not present)
				if (i > 10) break; // safety limit
			}
		}
		break;
	}
	default:
		break; // ignore unknown sequences
	}
}

void GUITerminal::applyAttrCode(int code)
{
	if (code == 0) {
		m_cur_attr = TermAttr{};
	} else if (code == 1) {
		m_cur_attr.bold = true;
	} else if (code == 22) {
		m_cur_attr.bold = false;
	} else if (code == 7) {
		m_cur_attr.reverse = true;
	} else if (code == 27) {
		m_cur_attr.reverse = false;
	} else if (code >= 30 && code <= 37) {
		m_cur_attr.fg = (u8)(code - 30);
	} else if (code == 39) {
		m_cur_attr.fg = TERM_COLOR_WHITE;
	} else if (code >= 40 && code <= 47) {
		m_cur_attr.bg = (u8)(code - 40);
	} else if (code == 49) {
		m_cur_attr.bg = TERM_COLOR_BLACK;
	}
}

void GUITerminal::clearRegion(u32 c0, u32 r0, u32 c1, u32 r1)
{
	for (u32 r = r0; r <= r1 && r < m_rows; ++r)
		for (u32 c = c0; c <= c1 && c < m_cols; ++c)
			cell(c, r) = TermCell{};
}

void GUITerminal::scrollUp()
{
	// Move rows 1..rows-1 up by one
	for (u32 r = 1; r < m_rows; ++r)
		for (u32 c = 0; c < m_cols; ++c)
			cell(c, r - 1) = cell(c, r);
	// Clear last row
	clearRegion(0, m_rows - 1, m_cols - 1, m_rows - 1);
	m_cur_row = m_rows - 1;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void GUITerminal::draw()
{
	if (!IsVisible) return;

	video::IVideoDriver *driver = Environment->getVideoDriver();
	gui::IGUISkin *skin = Environment->getSkin();

	// Font selection order:
	//   1. An explicit setOverrideFont() (rare, used for style overrides).
	//   2. A per-element font-size override (set via setFontSizeOverride),
	//      used by mods to implement +/- zoom buttons.
	//   3. Luanti's built-in mono font at the default size, driven by the
	//      global font_size setting.
	gui::IGUIFont *font = m_font;
	if (!font) {
		if (m_font_size_override > 0)
			font = g_fontengine->getFont(m_font_size_override, FM_Mono);
		else
			font = g_fontengine->getFont(FONT_SIZE_UNSPECIFIED, FM_Mono);
	}

	if (!font) font = skin->getFont(gui::EGDF_DEFAULT);
	if (!font) return;

	// Determine cell size from font
	// Use 'M' as the reference glyph for monospace width
	core::dimension2d<u32> char_dim = font->getDimension(L"M");
	u32 cell_w = char_dim.Width;
	u32 cell_h = char_dim.Height + 2; // small line spacing

	core::rect<s32> outer = AbsoluteRect;

	// Draw background for the whole terminal area
	driver->draw2DRectangle(s_palette[TERM_COLOR_BLACK], outer, &AbsoluteClippingRect);

	for (u32 row = 0; row < m_rows; ++row) {
		for (u32 col = 0; col < m_cols; ++col) {
			const TermCell &tc = cell(col, row);

			s32 x = outer.UpperLeftCorner.X + (s32)(col * cell_w);
			s32 y = outer.UpperLeftCorner.Y + (s32)(row * cell_h);
			core::rect<s32> cell_rect(x, y, x + cell_w, y + cell_h);

			// Skip cells outside clipping area
			if (!cell_rect.isRectCollided(AbsoluteClippingRect))
				continue;

			u8 fg_idx = tc.attr.fg;
			u8 bg_idx = tc.attr.bg;
			if (tc.attr.reverse) std::swap(fg_idx, bg_idx);

			video::SColor fg_color = s_palette[fg_idx & 7];
			video::SColor bg_color = s_palette[bg_idx & 7];

			// Bold: brighten fg
			if (tc.attr.bold) {
				fg_color.setRed  (std::min(255u, (u32)fg_color.getRed()   + 85));
				fg_color.setGreen(std::min(255u, (u32)fg_color.getGreen() + 85));
				fg_color.setBlue (std::min(255u, (u32)fg_color.getBlue()  + 85));
			}

			// Background cell (skip if default black to save draw calls)
			if (bg_idx != TERM_COLOR_BLACK || tc.attr.reverse)
				driver->draw2DRectangle(bg_color, cell_rect, &AbsoluteClippingRect);

			// Character
			if (tc.ch != L' ') {
				wchar_t wstr[2] = { tc.ch, L'\0' };
				font->draw(wstr, cell_rect, fg_color, false, false, &AbsoluteClippingRect);
			}
		}
	}

	// Draw cursor as a blinking underline block (simple: always visible)
	{
		s32 cx = outer.UpperLeftCorner.X + (s32)(m_cur_col * cell_w);
		s32 cy = outer.UpperLeftCorner.Y + (s32)(m_cur_row * cell_h) + (s32)cell_h - 2;
		core::rect<s32> cursor_rect(cx, cy, cx + (s32)cell_w, cy + 2);
		driver->draw2DRectangle(video::SColor(200, 200, 200, 200), cursor_rect, &AbsoluteClippingRect);
	}

	IGUIElement::draw();
}
void GUITerminal::setInputCallback(std::function<void(const std::string &)> cb)
{
	m_input_callback = std::move(cb);
}

// Encode a Unicode codepoint as UTF-8.
static std::string encode_utf8(wchar_t c)
{
	std::string out;
	u32 cp = (u32)c;
	if (cp < 0x80) {
		out += (char)cp;
	} else if (cp < 0x800) {
		out += (char)(0xC0 | (cp >> 6));
		out += (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += (char)(0xE0 | (cp >> 12));
		out += (char)(0x80 | ((cp >> 6) & 0x3F));
		out += (char)(0x80 | (cp & 0x3F));
	} else {
		out += (char)(0xF0 | (cp >> 18));
		out += (char)(0x80 | ((cp >> 12) & 0x3F));
		out += (char)(0x80 | ((cp >> 6) & 0x3F));
		out += (char)(0x80 | (cp & 0x3F));
	}
	return out;
}

bool GUITerminal::OnEvent(const SEvent &event)
{
	// On left mouse click: grab keyboard focus so we receive key events.
	if (event.EventType == EET_MOUSE_INPUT_EVENT) {
		if (event.MouseInput.Event == EMIE_LMOUSE_PRESSED_DOWN) {
			if (AbsoluteRect.isPointInside(
					core::position2di(event.MouseInput.X, event.MouseInput.Y))) {
				Environment->setFocus(this);
				return true;
			}
		}
		return false;
	}

	if (event.EventType != EET_KEY_INPUT_EVENT)
		return false;

	// Only react when we actually have focus.
	if (Environment->getFocus() != this)
		return false;

	if (!event.KeyInput.PressedDown)
		return false; // ignore key-up events

	if (!m_input_callback)
		return false;

	std::string data;

	bool ctrl = event.KeyInput.Control;

	// Ctrl+letter → control character (\x01–\x1a)
	if (ctrl) {
		EKEY_CODE key = event.KeyInput.Key;
		if (key >= KEY_KEY_A && key <= KEY_KEY_Z) {
			char cc = (char)(key - KEY_KEY_A + 1);
			data = std::string(1, cc);
			m_input_callback(data);
			return true;
		}
	}

	// Special keys → VT100 sequences
	switch (event.KeyInput.Key) {
		case KEY_RETURN:   data = "\r";       break;
		case KEY_BACK:     data = "\x7f";     break; // DEL (backspace)
		case KEY_TAB:      data = "\t";       break;
		case KEY_ESCAPE:   data = "\x1b";     break;
		case KEY_DELETE:   data = "\x1b[3~";  break;
		case KEY_HOME:     data = "\x1b[H";   break;
		case KEY_END:      data = "\x1b[F";   break;
		case KEY_PRIOR:    data = "\x1b[5~";  break; // Page Up
		case KEY_NEXT:     data = "\x1b[6~";  break; // Page Down
		case KEY_UP:       data = "\x1b[A";   break;
		case KEY_DOWN:     data = "\x1b[B";   break;
		case KEY_RIGHT:    data = "\x1b[C";   break;
		case KEY_LEFT:     data = "\x1b[D";   break;
		case KEY_F1:       data = "\x1bOP";   break;
		case KEY_F2:       data = "\x1bOQ";   break;
		case KEY_F3:       data = "\x1bOR";   break;
		case KEY_F4:       data = "\x1bOS";   break;
		case KEY_F5:       data = "\x1b[15~"; break;
		case KEY_F6:       data = "\x1b[17~"; break;
		case KEY_F7:       data = "\x1b[18~"; break;
		case KEY_F8:       data = "\x1b[19~"; break;
		case KEY_F9:       data = "\x1b[20~"; break;
		case KEY_F10:      data = "\x1b[21~"; break;
		case KEY_F11:      data = "\x1b[23~"; break;
		case KEY_F12:      data = "\x1b[24~"; break;
		default: break;
	}

	if (data.empty() && event.KeyInput.Char != 0) {
		// Printable character: encode as UTF-8
		data = encode_utf8(event.KeyInput.Char);
	}

	if (!data.empty()) {
		m_input_callback(data);
		return true;
	}

	return false;
}