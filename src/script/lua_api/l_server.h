// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "lua_api/l_base.h"
#include "serverterminal.h"

class ModApiServer : public ModApiBase
{
public:
	// The ScreenBuffer methods must be public so that the
	// C-function trampolines in l_server.cpp can forward to
	// them.  Otherwise the compiler complains that the static
	// member is private.
	static int l_screenbuffer_get_pos(lua_State *L);
	static int l_screenbuffer_get_element_name(lua_State *L);
	static int l_screenbuffer_get_formname(lua_State *L);
	static int l_screenbuffer_get_type(lua_State *L);
	static int l_screenbuffer_get_cols(lua_State *L);
	static int l_screenbuffer_get_rows(lua_State *L);
	static int l_screenbuffer_get_size(lua_State *L);
	static int l_screenbuffer_get_version(lua_State *L);
	static int l_screenbuffer_is_valid(lua_State *L);
	static int l_screenbuffer_set_cell(lua_State *L);
	static int l_screenbuffer_get_cell(lua_State *L);
	static int l_screenbuffer_clear(lua_State *L);
	static int l_screenbuffer_set_cursor(lua_State *L);
	static int l_screenbuffer_get_cursor(lua_State *L);
	static int l_screenbuffer_write_char(lua_State *L);
	static int l_screenbuffer_write_string(lua_State *L);
	static int l_screenbuffer_write_line(lua_State *L);
	static int l_screenbuffer_fill_rect(lua_State *L);
	static int l_screenbuffer_draw_box(lua_State *L);
	static int l_screenbuffer_scroll(lua_State *L);
	static int l_screenbuffer_serialize(lua_State *L);
	static int l_screenbuffer_deserialize(lua_State *L);
	static int l_screenbuffer_tostring(lua_State *L);
	static int l_screenbuffer_base64_encode(lua_State *L);
private:
	// request_shutdown([message], [reconnect])
	static int l_request_shutdown(lua_State *L);

	// get_server_status()
	static int l_get_server_status(lua_State *L);

	// get_server_uptime()
	static int l_get_server_uptime(lua_State *L);

	// get_server_max_lag()
	static int l_get_server_max_lag(lua_State *L);

	// get_worldpath()
	static int l_get_worldpath(lua_State *L);

	// get_mod_data_path()
	static int l_get_mod_data_path(lua_State *L);

	// is_singleplayer()
	static int l_is_singleplayer(lua_State *L);

	// get_current_modname()
	static int l_get_current_modname(lua_State *L);

	// get_modpath(modname)
	static int l_get_modpath(lua_State *L);

	// get_modnames()
	// the returned list is sorted alphabetically for you
	static int l_get_modnames(lua_State *L);

	// get_game_info()
	static int l_get_game_info(lua_State *L);

	// print(text)
	static int l_print(lua_State *L);

	// chat_send_all(text)
	static int l_chat_send_all(lua_State *L);

	// chat_send_player(name, text)
	static int l_chat_send_player(lua_State *L);

	// show_formspec(playername,formname,formspec)
	static int l_show_formspec(lua_State *L);

	// ScreenBuffer constructor.  (Other ScreenBuffer methods
	// are declared in the public: section above so the C-function
	// trampolines in l_server.cpp can call them.)
	static int l_create_screenbuffer(lua_State *L);

	// sound_play(spec, parameters)
	static int l_sound_play(lua_State *L);

	// sound_stop(handle)
	static int l_sound_stop(lua_State *L);

	// sound_fade(handle, step, gain)
	static int l_sound_fade(lua_State *L);

	// dynamic_add_media(filepath)
	static int l_dynamic_add_media(lua_State *L);

	// get_player_privs(name, text)
	static int l_get_player_privs(lua_State *L);

	// get_player_ip()
	static int l_get_player_ip(lua_State *L);

	// get_player_information(name)
	static int l_get_player_information(lua_State *L);

	// get_player_window_information(name)
	static int l_get_player_window_information(lua_State *L);

	// get_ban_list()
	static int l_get_ban_list(lua_State *L);

	// get_ban_description()
	static int l_get_ban_description(lua_State *L);

	// ban_player()
	static int l_ban_player(lua_State *L);

	// unban_player_or_ip()
	static int l_unban_player_or_ip(lua_State *L);

	// disconnect_player(name[, reason[, reconnect]]) -> success
	static int l_disconnect_player(lua_State *L);

	// remove_player(name)
	static int l_remove_player(lua_State *L);

	// notify_authentication_modified(name)
	static int l_notify_authentication_modified(lua_State *L);

	// register_async_dofile(path)
	static int l_register_async_dofile(lua_State *L);

	// register_mapgen_script(path)
	static int l_register_mapgen_script(lua_State *L);

	// serialize_roundtrip(obj)
	static int l_serialize_roundtrip(lua_State *L);

public:
	static void Initialize(lua_State *L, int top);
	static void InitializeAsync(lua_State *L, int top);
};
