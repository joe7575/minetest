// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2024 Luanti contributors

#include "guiTerminal.h"

#include <IGUIEnvironment.h>
#include <IGUISkin.h>
#include <IGUIFont.h>
#include <IVideoDriver.h>
#include <algorithm>
#include <cstring>
#include "client/fontengine.h"

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

GUITerminal::TerminalState GUITerminal::saveState() const
{
	return { m_cells, m_cur_col, m_cur_row, m_cur_attr };
}

void GUITerminal::restoreState(const TerminalState &s)
{
	// Restore cells up to the current grid size
	u32 n = std::min((u32)s.cells.size(), m_cols * m_rows);
	std::copy(s.cells.begin(), s.cells.begin() + n, m_cells.begin());
	m_cur_col  = std::min(s.cur_col, m_cols - 1);
	m_cur_row  = std::min(s.cur_row, m_rows - 1);
	m_cur_attr = s.cur_attr;
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

	// Prefer an explicit override font, then Luanti's built-in mono font
	gui::IGUIFont *font = m_font
		? m_font
		: g_fontengine->getFont(FONT_SIZE_UNSPECIFIED, FM_Mono);

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
