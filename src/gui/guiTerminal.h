// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2024 Luanti contributors

#pragma once

#include <IGUIElement.h>
#include <SColor.h>
#include <string>
#include <vector>
#include <functional>

namespace gui { class IGUIFont; }

// Maximum terminal dimensions
#define TERMINAL_MAX_COLS 240
#define TERMINAL_MAX_ROWS 60

// VT100/ANSI color indices (standard 8 colors)
enum TermColor : u8 {
	TERM_COLOR_BLACK   = 0,
	TERM_COLOR_RED     = 1,
	TERM_COLOR_GREEN   = 2,
	TERM_COLOR_YELLOW  = 3,
	TERM_COLOR_BLUE    = 4,
	TERM_COLOR_MAGENTA = 5,
	TERM_COLOR_CYAN    = 6,
	TERM_COLOR_WHITE   = 7,
};

struct TermAttr {
	u8 fg = TERM_COLOR_WHITE;
	u8 bg = TERM_COLOR_BLACK;
	bool bold      = false;
	bool reverse   = false;
};

struct TermCell {
	wchar_t ch   = L' ';
	TermAttr attr;
};

/*
 * GUITerminal – a VT100/ANSI terminal emulator widget for Irrlicht.
 *
 * The server sends raw bytes (including escape sequences) via
 * minetest.send_terminal_data().  The client calls feed() which updates
 * the internal cell grid.  draw() renders the grid each frame using a
 * monospaced font.
 *
 * Supported sequences (minimal VT100 subset):
 *   \r  \n  \b  \t  \a (bell ignored)
 *   ESC[A/B/C/D          cursor up/down/right/left
 *   ESC[{r};{c}H / ESC[H cursor position (1-based)
 *   ESC[{r};{c}f          same as H
 *   ESC[2J               clear screen + home
 *   ESC[J / ESC[0J       clear from cursor to end of screen
 *   ESC[1J               clear from start of screen to cursor
 *   ESC[K / ESC[0K       clear from cursor to end of line
 *   ESC[1K               clear from start of line to cursor
 *   ESC[2K               clear entire line
 *   ESC[m / ESC[0m       reset attributes
 *   ESC[1m               bold on
 *   ESC[22m              bold off
 *   ESC[7m               reverse video
 *   ESC[27m              reverse off
 *   ESC[30m-ESC[37m      set foreground color
 *   ESC[39m              default foreground
 *   ESC[40m-ESC[47m      set background color
 *   ESC[49m              default background
 *   ESC[c                reset (same as full clear + attribute reset)
 */
class GUITerminal : public gui::IGUIElement
{
public:
	GUITerminal(gui::IGUIEnvironment *env, gui::IGUIElement *parent,
		s32 id, core::rect<s32> rect, u32 cols, u32 rows);

	~GUITerminal() override;

	// Feed raw bytes (possibly containing VT100 sequences) into the terminal.
	void feed(const std::string &data);

	// Reset the terminal (clear screen, home cursor, reset attributes).
	void reset();

	// Snapshot / restore full terminal state (used to survive formspec regeneration)
	struct TerminalState {
		std::vector<TermCell> cells;
		u32 cur_col, cur_row;
		TermAttr cur_attr;
	};
	TerminalState saveState() const;
	void restoreState(const TerminalState &s);

	// IGUIElement interface
	void draw() override;
	bool OnEvent(const SEvent &event) override;

	// Set callback that is invoked with raw key data whenever a key is pressed.
	// The data is already encoded as a VT100/UTF-8 byte string.
	void setInputCallback(std::function<void(const std::string &)> cb);

	void setOverrideFont(gui::IGUIFont *font);

private:
	std::function<void(const std::string &)> m_input_callback;
	u32 m_cols;
	u32 m_rows;
	u32 m_cur_col = 0;   // 0-based
	u32 m_cur_row = 0;   // 0-based
	TermAttr m_cur_attr;

	std::vector<TermCell> m_cells; // row-major: index = row*cols + col

	gui::IGUIFont *m_font = nullptr;

	// VT100 parser state machine
	enum class ParseState {
		NORMAL,
		ESC,          // received ESC
		CSI,          // received ESC [
		CSI_PARAM,    // collecting digits/semicolons
	} m_parse_state = ParseState::NORMAL;

	std::string m_csi_params; // accumulated CSI parameter string

	// Internal helpers
	TermCell &cell(u32 col, u32 row) { return m_cells[row * m_cols + col]; }
	void clearRegion(u32 c0, u32 r0, u32 c1, u32 r1);
	void scrollUp();
	void processCsi();
	void applyAttrCode(int code);

	static const video::SColor s_palette[8];
};
