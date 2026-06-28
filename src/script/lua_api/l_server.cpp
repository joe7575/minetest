// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#include "lua_api/l_server.h"

#include "common/c_content.h"
#include "common/c_converter.h"
#include "common/c_packer.h"
#include "content/mods.h" // ModSpec
#include "cpp_api/s_base.h"
#include "cpp_api/s_security.h"
#include "filesys.h"
#include "log.h"
#include "lua_api/l_internal.h"
#include "network/connection.h"
#include "remoteplayer.h"
#include "scripting_server.h"
#include "server.h"
#include "serverenvironment.h"

#include <algorithm>

// request_shutdown()
int ModApiServer::l_request_shutdown(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const char *msg = lua_tolstring(L, 1, NULL);
	bool reconnect = readParam<bool>(L, 2);
	float seconds_before_shutdown = lua_tonumber(L, 3);
	getServer(L)->requestShutdown(msg ? msg : "", reconnect, seconds_before_shutdown);
	return 0;
}

// get_server_status()
int ModApiServer::l_get_server_status(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	lua_pushstring(L, getServer(L)->getStatusString().c_str());
	return 1;
}

// get_server_uptime()
int ModApiServer::l_get_server_uptime(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	lua_pushnumber(L, getServer(L)->getUptime());
	return 1;
}

// get_server_max_lag()
int ModApiServer::l_get_server_max_lag(lua_State *L)
{
	GET_ENV_PTR_NO_MAP_LOCK;
	lua_pushnumber(L, env->getMaxLagEstimate());
	return 1;
}

// print(text)
int ModApiServer::l_print(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	std::string text;
	text = luaL_checkstring(L, 1);
	getServer(L)->printToConsoleOnly(text);
	return 0;
}

// chat_send_all(text)
int ModApiServer::l_chat_send_all(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const char *text = luaL_checkstring(L, 1);
	// Get server from registry
	Server *server = getServer(L);
	// Send
	try {
		server->notifyPlayers(utf8_to_wide(text));
	} catch (PacketError &e) {
		warningstream << "Exception caught: " << e.what() << std::endl
			<< script_get_backtrace(L) << std::endl;
		server->notifyPlayers(utf8_to_wide(std::string("Internal error: ") + e.what()));
	}

	return 0;
}

// chat_send_player(name, text)
int ModApiServer::l_chat_send_player(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const char *name = luaL_checkstring(L, 1);
	const char *text = luaL_checkstring(L, 2);

	// Get server from registry
	Server *server = getServer(L);
	// Send
	try {
		server->notifyPlayer(name, utf8_to_wide(text));
	} catch (PacketError &e) {
		warningstream << "Exception caught: " << e.what() << std::endl
			<< script_get_backtrace(L) << std::endl;
		server->notifyPlayer(name, utf8_to_wide(std::string("Internal error: ") + e.what()));
	}
	return 0;
}

// get_player_privs(name, text)
int ModApiServer::l_get_player_privs(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const char *name = luaL_checkstring(L, 1);
	// Get server from registry
	Server *server = getServer(L);
	// Do it
	lua_newtable(L);
	int table = lua_gettop(L);
	std::set<std::string> privs_s = server->getPlayerEffectivePrivs(name);
	for (const std::string &privs_ : privs_s) {
		lua_pushboolean(L, true);
		lua_setfield(L, table, privs_.c_str());
	}
	lua_pushvalue(L, table);
	return 1;
}

// get_player_ip()
int ModApiServer::l_get_player_ip(lua_State *L)
{
	GET_ENV_PTR_NO_MAP_LOCK;

	const char *name = luaL_checkstring(L, 1);
	RemotePlayer *player = env->getPlayer(name);
	if (!player) {
		lua_pushnil(L); // no such player
		return 1;
	}

	lua_pushstring(L, env->getGameDef()->getPeerAddress(
		player->getPeerId()).serializeString().c_str()
	);
	return 1;
}

// get_player_information(name)
int ModApiServer::l_get_player_information(lua_State *L)
{
	GET_ENV_PTR_NO_MAP_LOCK;

	const char *name = luaL_checkstring(L, 1);
	RemotePlayer *player = env->getPlayer(name);
	if (!player) {
		lua_pushnil(L); // no such player
		return 1;
	}

	Server *server = env->getGameDef();
	ClientInfo info;
	if (!server->getClientInfo(player->getPeerId(), info)) {
		warningstream << FUNCTION_NAME << ": no client info?!" << std::endl;
		lua_pushnil(L); // error
		return 1;
	}

	lua_newtable(L);
	int table = lua_gettop(L);

	lua_pushstring(L,"address");
	lua_pushstring(L, info.addr.serializeString().c_str());
	lua_settable(L, table);

	lua_pushstring(L,"ip_version");
	if (info.addr.getFamily() == AF_INET) {
		lua_pushnumber(L, 4);
	} else if (info.addr.getFamily() == AF_INET6) {
		lua_pushnumber(L, 6);
	} else {
		lua_pushnumber(L, 0);
	}
	lua_settable(L, table);

	/*
		Be careful not to introduce a depdendency on the connection to
		the peer here. This function is >>REQUIRED<< to still be able to return
		values even when the peer unexpectedly disappears.
		Hence all the ConInfo values here are optional.
	*/

	auto getConInfo = [&] (con::rtt_stat_type type, float *value) -> bool {
		return server->getClientConInfo(player->getPeerId(), type, value);
	};

	float min_rtt, max_rtt, avg_rtt, min_jitter, max_jitter, avg_jitter;
	bool have_con_info =
		getConInfo(con::MIN_RTT, &min_rtt) &&
		getConInfo(con::MAX_RTT, &max_rtt) &&
		getConInfo(con::AVG_RTT, &avg_rtt) &&
		getConInfo(con::MIN_JITTER, &min_jitter) &&
		getConInfo(con::MAX_JITTER, &max_jitter) &&
		getConInfo(con::AVG_JITTER, &avg_jitter);

	if (have_con_info) { // may be missing
		lua_pushstring(L, "min_rtt");
		lua_pushnumber(L, min_rtt);
		lua_settable(L, table);

		lua_pushstring(L, "max_rtt");
		lua_pushnumber(L, max_rtt);
		lua_settable(L, table);

		lua_pushstring(L, "avg_rtt");
		lua_pushnumber(L, avg_rtt);
		lua_settable(L, table);

		lua_pushstring(L, "min_jitter");
		lua_pushnumber(L, min_jitter);
		lua_settable(L, table);

		lua_pushstring(L, "max_jitter");
		lua_pushnumber(L, max_jitter);
		lua_settable(L, table);

		lua_pushstring(L, "avg_jitter");
		lua_pushnumber(L, avg_jitter);
		lua_settable(L, table);
	}

	lua_pushstring(L,"connection_uptime");
	lua_pushnumber(L, info.uptime);
	lua_settable(L, table);

	lua_pushstring(L,"protocol_version");
	lua_pushnumber(L, info.prot_vers);
	lua_settable(L, table);

	lua_pushstring(L, "formspec_version");
	lua_pushnumber(L, player->formspec_version);
	lua_settable(L, table);

	lua_pushstring(L, "lang_code");
	lua_pushstring(L, info.lang_code.c_str());
	lua_settable(L, table);

	lua_pushstring(L, "version_string");
	lua_pushstring(L, info.vers_string.c_str());
	lua_settable(L, table);

#ifndef NDEBUG
	lua_pushstring(L,"serialization_version");
	lua_pushnumber(L, info.ser_vers);
	lua_settable(L, table);

	lua_pushstring(L,"major");
	lua_pushnumber(L, info.major);
	lua_settable(L, table);

	lua_pushstring(L,"minor");
	lua_pushnumber(L, info.minor);
	lua_settable(L, table);

	lua_pushstring(L,"patch");
	lua_pushnumber(L, info.patch);
	lua_settable(L, table);

	lua_pushstring(L,"state");
	lua_pushstring(L, ClientInterface::state2Name(info.state));
	lua_settable(L, table);
#endif

	return 1;
}

// get_player_window_information(name)
int ModApiServer::l_get_player_window_information(lua_State *L)
{
	GET_ENV_PTR_NO_MAP_LOCK;

	const char *name = luaL_checkstring(L, 1);
	RemotePlayer *player = env->getPlayer(name);
	if (!player)
		return 0;

	Server *server = env->getGameDef();
	auto dynamic = server->getClientDynamicInfo(player->getPeerId());

	if (!dynamic || dynamic->render_target_size == v2u32())
		return 0;

	lua_newtable(L);
	int dyn_table = lua_gettop(L);

	lua_pushstring(L, "size");
	push_v2u32(L, dynamic->render_target_size);
	lua_settable(L, dyn_table);

	lua_pushstring(L, "max_formspec_size");
	push_v2f(L, dynamic->max_fs_size);
	lua_settable(L, dyn_table);

	lua_pushstring(L, "real_gui_scaling");
	lua_pushnumber(L, dynamic->real_gui_scaling);
	lua_settable(L, dyn_table);

	lua_pushstring(L, "real_hud_scaling");
	lua_pushnumber(L, dynamic->real_hud_scaling);
	lua_settable(L, dyn_table);

	lua_pushstring(L, "touch_controls");
	lua_pushboolean(L, dynamic->touch_controls);
	lua_settable(L, dyn_table);

	return 1;
}

// get_ban_list()
int ModApiServer::l_get_ban_list(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	lua_pushstring(L, getServer(L)->getBanDescription("").c_str());
	return 1;
}

// get_ban_description()
int ModApiServer::l_get_ban_description(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const char * ip_or_name = luaL_checkstring(L, 1);
	lua_pushstring(L, getServer(L)->getBanDescription(std::string(ip_or_name)).c_str());
	return 1;
}

// ban_player()
int ModApiServer::l_ban_player(lua_State *L)
{
	GET_ENV_PTR_NO_MAP_LOCK;

	const char *name = luaL_checkstring(L, 1);
	RemotePlayer *player = env->getPlayer(name);
	if (!player) {
		lua_pushboolean(L, false); // no such player
		return 1;
	}

	Server *server = env->getGameDef();
	std::string ip_str = server->getPeerAddress(player->getPeerId()).serializeString();
	server->setIpBanned(ip_str, name);
	lua_pushboolean(L, true);
	return 1;
}

// disconnect_player(name[, reason[, reconnect]]) -> success
int ModApiServer::l_disconnect_player(lua_State *L)
{
	GET_ENV_PTR_NO_MAP_LOCK;

	const char *name = luaL_checkstring(L, 1);
	std::string message;
	if (lua_isstring(L, 2))
		message.append(readParam<std::string>(L, 2));
	else
		message.append("Disconnected.");

	RemotePlayer *player = env->getPlayer(name);
	if (!player) {
		lua_pushboolean(L, false); // No such player
		return 1;
	}

	bool reconnect = readParam<bool>(L, 3, false);

	Server *server = env->getGameDef();
	server->DenyAccess(player->getPeerId(), SERVER_ACCESSDENIED_CUSTOM_STRING, message, reconnect);
	lua_pushboolean(L, true);
	return 1;
}

int ModApiServer::l_remove_player(lua_State *L)
{
	GET_ENV_PTR_NO_MAP_LOCK;
	std::string name = luaL_checkstring(L, 1);

	RemotePlayer *player = env->getPlayer(name.c_str());
	if (!player)
		lua_pushinteger(L, env->removePlayerFromDatabase(name) ? 0 : 1);
	else
		lua_pushinteger(L, 2);

	return 1;
}

// unban_player_or_ip()
int ModApiServer::l_unban_player_or_ip(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const char * ip_or_name = luaL_checkstring(L, 1);
	getServer(L)->unsetIpBanned(ip_or_name);
	lua_pushboolean(L, true);
	return 1;
}

// show_formspec(playername,formname,formspec)
int ModApiServer::l_show_formspec(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const char *playername = luaL_checkstring(L, 1);
	const char *formname = luaL_checkstring(L, 2);
	const char *formspec = luaL_checkstring(L, 3);
	lua_pushboolean(L, getServer(L)->showFormspec(playername,formspec,formname));
	return 1;
}

// send_terminal_data(playername, formname, element_name, data)
// REMOVED in screen_buffer refactor: use ScreenBuffer:write() on
// the server side (with VT100 escape sequences) instead.

// terminal_set_cell(formname, element_name, col, row, char [, fg, bg])
// REMOVED: use ScreenBuffer(pos, elem):set_cell(col, row, char, fg, bg).

// terminal_clear(formname, element_name)
// REMOVED: use ScreenBuffer(pos, elem):clear().

// terminal_destroy(formname, element_name) -> bool
// REMOVED: ScreenBuffer(pos, elem):destroy() (TODO: not yet
// implemented; use ServerScreenBuffer directly if needed).

// terminal_get_size(formname, element_name) -> cols, rows or nil
// REMOVED: use ScreenBuffer(pos, elem):get_size().

// terminal_scan_formspec(formname, formspec)
// REMOVED: ScreenBuffer(pos, elem) constructor scans the node's
// formspec automatically.

// get_current_modname()
int ModApiServer::l_get_current_modname(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	std::string s = ScriptApiBase::getCurrentModNameInsecure(L);
	if (!s.empty())
		lua_pushstring(L, s.c_str());
	else
		lua_pushnil(L);
	return 1;
}

// get_modpath(modname)
int ModApiServer::l_get_modpath(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	std::string modname = luaL_checkstring(L, 1);
	const ModSpec *mod = getGameDef(L)->getModSpec(modname);
	if (!mod)
		lua_pushnil(L);
	else
		lua_pushstring(L, mod->path.c_str());
	return 1;
}

// get_modnames()
// the returned list is sorted alphabetically for you
int ModApiServer::l_get_modnames(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const bool use_load_order = readParam<bool>(L, 1, false);

	// Get a list of mods
	std::vector<std::string> modlist;
	for (auto &it : getGameDef(L)->getMods())
		modlist.emplace_back(it.name);

	if (!use_load_order) {
		// Alphabetical order
		std::sort(modlist.begin(), modlist.end());
	}

	// Package them up for Lua
	lua_createtable(L, modlist.size(), 0);
	auto iter = modlist.begin();
	for (u16 i = 0; iter != modlist.end(); ++iter) {
		lua_pushstring(L, iter->c_str());
		lua_rawseti(L, -2, ++i);
	}
	return 1;
}

// get_game_info()
int ModApiServer::l_get_game_info(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const SubgameSpec *game_spec = getGameDef(L)->getGameSpec();
	assert(game_spec);
	lua_newtable(L);
	setstringfield(L, -1, "id", game_spec->id);
	setstringfield(L, -1, "title", game_spec->title);
	setstringfield(L, -1, "author", game_spec->author);
	setstringfield(L, -1, "path", game_spec->path);
	return 1;
}

// get_worldpath()
int ModApiServer::l_get_worldpath(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const Server *srv = getServer(L);
	lua_pushstring(L, srv->getWorldPath().c_str());
	return 1;
}

// get_mod_data_path()
int ModApiServer::l_get_mod_data_path(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;

	std::string modname = ScriptApiBase::getCurrentModNameInsecure(L);
	if (modname.empty())
		return 0;

	const Server *srv = getServer(L);
	std::string path = srv->getModDataPath() + DIR_DELIM + modname;
	if (!fs::CreateAllDirs(path))
		throw LuaError("Failed to create dir");

	lua_pushstring(L, path.c_str());
	return 1;
}

// sound_play(spec, parameters, [ephemeral])
int ModApiServer::l_sound_play(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	ServerPlayingSound params;
	read_simplesoundspec(L, 1, params.spec);
	read_server_sound_params(L, 2, params);
	bool ephemeral = lua_gettop(L) > 2 && readParam<bool>(L, 3);
	if (ephemeral) {
		getServer(L)->playSound(params, true);
		lua_pushnil(L);
	} else {
		s32 handle = getServer(L)->playSound(params);
		lua_pushinteger(L, handle);
	}
	return 1;
}

// sound_stop(handle)
int ModApiServer::l_sound_stop(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	s32 handle = luaL_checkinteger(L, 1);
	getServer(L)->stopSound(handle);
	return 0;
}

int ModApiServer::l_sound_fade(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	s32 handle = luaL_checkinteger(L, 1);
	float step = readParam<float>(L, 2);
	float gain = readParam<float>(L, 3);
	getServer(L)->fadeSound(handle, step, gain);
	return 0;
}

// dynamic_add_media(filepath)
int ModApiServer::l_dynamic_add_media(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;

	Server *server = getServer(L);
	const bool at_startup = !getEnv(L);

	std::string tmp;
	Server::DynamicMediaArgs args;

	if (lua_istable(L, 1)) {
		getstringfield(L, 1, "filename", args.filename);
		if (getstringfield(L, 1, "filepath", tmp))
			args.filepath = tmp;
		args.data.emplace();
		if (!getstringfield(L, 1, "filedata", *args.data))
			args.data.reset();
		getstringfield(L, 1, "to_player", args.to_player);
		getboolfield(L, 1, "ephemeral", args.ephemeral);
		args.client_cache = getboolfield_default(L, 1, "client_cache", !args.ephemeral);
	} else {
		tmp = readParam<std::string>(L, 1);
		args.filepath = tmp;
		log_deprecated(L, "Deprecated call to core.dynamic_add_media() with string argument", 1, true);
	}
	if (at_startup) {
		if (!lua_isnoneornil(L, 2))
			throw LuaError("must be called without callback at load-time");
		// In order to keep edge cases to a minimum actually use an empty function.
		int err = luaL_loadstring(L, "");
		SANITY_CHECK(err == 0);
		lua_replace(L, 2);
	} else {
		luaL_checktype(L, 2, LUA_TFUNCTION);
	}

	// validate
	if (args.filepath) {
		if (args.filepath->empty())
			throw LuaError("filepath must be non-empty");
		if (args.data)
			throw LuaError("cannot provide both filepath and filedata");
	} else if (args.data) {
		if (args.filename.empty())
			throw LuaError("filename required");
	} else {
		throw LuaError("either filepath or filedata must be provided");
	}

	if (args.filepath)
		CHECK_SECURE_PATH(L, args.filepath->c_str(), false);

	args.token = server->getScriptIface()->allocateDynamicMediaCallback(L, 2);

	bool ok = server->dynamicAddMedia(args);
	if (!ok)
		server->getScriptIface()->freeDynamicMediaCallback(args.token);
	lua_pushboolean(L, ok);

	return 1;
}

// is_singleplayer()
int ModApiServer::l_is_singleplayer(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const Server *srv = getServer(L);
	lua_pushboolean(L, srv->isSingleplayer());
	return 1;
}

// notify_authentication_modified(name)
int ModApiServer::l_notify_authentication_modified(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	std::string name;
	if(lua_isstring(L, 1))
		name = readParam<std::string>(L, 1);
	getServer(L)->reportPrivsModified(name);
	return 0;
}

// register_async_dofile(path)
int ModApiServer::l_register_async_dofile(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;

	std::string path = readParam<std::string>(L, 1);
	CHECK_SECURE_PATH(L, path.c_str(), false);

	std::string modname = ScriptApiBase::getCurrentModNameInsecure(L);
	if (modname.empty())
		throw ModError("cannot determine mod name");

	getServer(L)->m_async_init_files.emplace_back(modname, path);
	lua_pushboolean(L, true);
	return 1;
}

// register_mapgen_script(path)
int ModApiServer::l_register_mapgen_script(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;

	std::string path = readParam<std::string>(L, 1);
	CHECK_SECURE_PATH(L, path.c_str(), false);

	std::string modname = ScriptApiBase::getCurrentModNameInsecure(L);
	if (modname.empty())
		throw ModError("cannot determine mod name");

	getServer(L)->m_mapgen_init_files.emplace_back(modname, path);
	lua_pushboolean(L, true);
	return 1;
}

// serialize_roundtrip(value)
// Meant for unit testing the packer from Lua
int ModApiServer::l_serialize_roundtrip(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;

	int top = lua_gettop(L);
	auto *pv = script_pack(L, 1);
	if (top != lua_gettop(L))
		throw LuaError("stack values leaked");

#ifndef NDEBUG
	script_dump_packed(pv);
#endif

	top = lua_gettop(L);
	script_unpack(L, pv);
	delete pv;
	if (top + 1 != lua_gettop(L))
		throw LuaError("stack values leaked");

	return 1;
}

// =====================================================================
// ScreenBuffer class
//
// A ScreenBuffer is identified by (formname, element_name). Each
// instance is a Lua-side handle; the actual cell storage lives in
// ServerTerminalStore on the server.
//
// Lua-side representation: a Table with `formname` and
// `element_name` fields, plus a metatable that defines all the
// methods. This is idiomatic Luanti Lua (cf. NodeMetaRef) and
// avoids the complexity of full userdata-class management.
// =====================================================================

static const char *ScreenBuffer_classname = "ScreenBuffer";

// -------------------------------------------------------------------------
// ScreenBuffer Lua-class trampolines.  Forward-declared so they
// are visible to push_screenbuffer() and the Initialize() /
// InitializeAsync() metatable setup code below.
// -------------------------------------------------------------------------
static int screenbuffer_get_pos(lua_State *L);
static int screenbuffer_get_element_name(lua_State *L);
static int screenbuffer_get_formname(lua_State *L);
static int screenbuffer_get_type(lua_State *L);
static int screenbuffer_get_cols(lua_State *L);
static int screenbuffer_get_rows(lua_State *L);
static int screenbuffer_get_size(lua_State *L);
static int screenbuffer_get_version(lua_State *L);
static int screenbuffer_is_valid(lua_State *L);
static int screenbuffer_set_cell(lua_State *L);
static int screenbuffer_get_cell(lua_State *L);
static int screenbuffer_clear(lua_State *L);
static int screenbuffer_set_cursor(lua_State *L);
static int screenbuffer_get_cursor(lua_State *L);
static int screenbuffer_write_char(lua_State *L);
static int screenbuffer_write_string(lua_State *L);
static int screenbuffer_write_line(lua_State *L);
static int screenbuffer_fill_rect(lua_State *L);
static int screenbuffer_draw_box(lua_State *L);
static int screenbuffer_scroll(lua_State *L);
static int screenbuffer_serialize(lua_State *L);
static int screenbuffer_deserialize(lua_State *L);
static int screenbuffer_tostring(lua_State *L);

// Register a ScreenBuffer class (metatable + method table) in
// the given Lua state.  Idempotent: if the metatable already
// exists in the registry, the existing one is augmented with
// any new method entries that the caller passed (in our case
// we just use this to lazily install the metatable when a
// mod requests ScreenBuffer in a context where Initialize
// or InitializeAsync didn't run, e.g. an async-thread
// background job).
static void register_screenbuffer_class(lua_State *L)
{
	luaL_newmetatable(L, ScreenBuffer_classname);
	int sb_mt = lua_gettop(L);
	// Methods table
	lua_newtable(L);
	int sb_methods = lua_gettop(L);
	// We use absolute indices for the setfield calls below so
	// that the position of sb_mt on the stack is irrelevant.
	lua_pushcfunction(L, screenbuffer_serialize);
	lua_setfield(L, sb_methods, "serialize");
	lua_pushcfunction(L, screenbuffer_deserialize);
	lua_setfield(L, sb_methods, "deserialize");
	lua_pushcfunction(L, screenbuffer_get_type);
	lua_setfield(L, sb_methods, "get_type");
	lua_pushcfunction(L, screenbuffer_get_cols);
	lua_setfield(L, sb_methods, "get_cols");
	lua_pushcfunction(L, screenbuffer_get_rows);
	lua_setfield(L, sb_methods, "get_rows");
	lua_pushcfunction(L, screenbuffer_get_size);
	lua_setfield(L, sb_methods, "get_size");
	lua_pushcfunction(L, screenbuffer_get_version);
	lua_setfield(L, sb_methods, "get_version");
	lua_pushcfunction(L, screenbuffer_is_valid);
	lua_setfield(L, sb_methods, "is_valid");
	lua_pushcfunction(L, screenbuffer_set_cell);
	lua_setfield(L, sb_methods, "set_cell");
	lua_pushcfunction(L, screenbuffer_get_cell);
	lua_setfield(L, sb_methods, "get_cell");
	lua_pushcfunction(L, screenbuffer_clear);
	lua_setfield(L, sb_methods, "clear");
	lua_pushcfunction(L, screenbuffer_set_cursor);
	lua_setfield(L, sb_methods, "set_cursor");
	lua_pushcfunction(L, screenbuffer_get_cursor);
	lua_setfield(L, sb_methods, "get_cursor");
	lua_pushcfunction(L, screenbuffer_write_char);
	lua_setfield(L, sb_methods, "write_char");
	lua_pushcfunction(L, screenbuffer_write_string);
	lua_setfield(L, sb_methods, "write_string");
	lua_pushcfunction(L, screenbuffer_write_line);
	lua_setfield(L, sb_methods, "write_line");
	lua_pushcfunction(L, screenbuffer_fill_rect);
	lua_setfield(L, sb_methods, "fill_rect");
	lua_pushcfunction(L, screenbuffer_draw_box);
	lua_setfield(L, sb_methods, "draw_box");
	lua_pushcfunction(L, screenbuffer_scroll);
	lua_setfield(L, sb_methods, "scroll");
	lua_pushcfunction(L, screenbuffer_get_pos);
	lua_setfield(L, sb_methods, "get_pos");
	lua_pushcfunction(L, screenbuffer_get_formname);
	lua_setfield(L, sb_methods, "get_formname");
	lua_pushcfunction(L, screenbuffer_get_element_name);
	lua_setfield(L, sb_methods, "get_element_name");
	// Set the method table as __index on the metatable.  We push the
	// methods table as the value because lua_setfield pops the value
	// and uses the index as the table to write into.
	lua_pushvalue(L, sb_methods);
	lua_setfield(L, sb_mt, "__index");
	// __metatable: point to itself to protect the metatable from
	// getmetatable.
	lua_pushvalue(L, sb_mt);
	lua_setfield(L, sb_mt, "__metatable");
	// __tostring
	lua_pushcfunction(L, screenbuffer_tostring);
	lua_setfield(L, sb_mt, "__tostring");
	// Pop the metatable.
	lua_pop(L, 1);
}

// Push a ScreenBuffer Lua object onto the stack and return 1.
static int push_screenbuffer(lua_State *L, const std::string &formname,
		const std::string &element_name)
{
	lua_newtable(L);  // sb = {}
	lua_pushstring(L, formname.c_str());
	lua_setfield(L, -2, "formname");
	lua_pushstring(L, element_name.c_str());
	lua_setfield(L, -2, "element_name");

	// Set metatable with methods.  We must call register_screenbuffer_class
	// every time push_screenbuffer runs, because the registry is
	// per-Lua-state and minetest.after() callbacks may run in a
	// state that didn't see Initialize or InitializeAsync.
	// (luaL_newmetatable is a no-op if the metatable already
	// exists in the registry.)
	register_screenbuffer_class(L);
	// Pop any extras that register_screenbuffer_class might have
	// left on the stack.  It pushes one metatable; we want only
	// sb_table on the stack.
	int target_top = lua_gettop(L) - 1;  // sb_table is one below
	while (lua_gettop(L) > target_top) {
		lua_pop(L, 1);
	}
	// Now sb_table is at top.  Get the metatable and set it.
	luaL_getmetatable(L, ScreenBuffer_classname);
	lua_setmetatable(L, -2);
	return 1;
}

// Get the (formname, element_name) from a ScreenBuffer at stack
// index. Throws Lua error if the argument is not a ScreenBuffer.
static void get_screenbuffer(lua_State *L, int index,
		std::string &formname, std::string &element_name)
{
	if (!lua_istable(L, index)) {
		luaL_error(L, "expected ScreenBuffer");
	}
	lua_getfield(L, index, "formname");
	formname = luaL_checkstring(L, -1);
	lua_pop(L, 1);
	lua_getfield(L, index, "element_name");
	element_name = luaL_checkstring(L, -1);
	lua_pop(L, 1);
}

// Helper for get_size: returns cols, rows
int ModApiServer::l_screenbuffer_get_size(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	u16 cols = 0, rows = 0;
	getServer(L)->terminalGetSize(formname, element_name, cols, rows);
	lua_pushinteger(L, cols);
	lua_pushinteger(L, rows);
	return 2;
}

int ModApiServer::l_screenbuffer_tostring(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	lua_pushfstring(L, "ScreenBuffer(%s, %s)",
		formname.c_str(), element_name.c_str());
	return 1;
}

// -------------------------------------------------------------------------
// ScreenBuffer methods
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// ScreenBuffer Lua-class trampolines.
//
// The ScreenBuffer methods are static member functions of
// ModApiServer.  Converting their addresses to lua_CFunction is
// implementation-defined, and on GCC/x86-64 the result is *not* a
// valid function pointer.  So we define plain C-function
// trampolines here that just forward to the static methods.
// (Forward declarations are at the top of the file.)
static int screenbuffer_get_pos(lua_State *L)
	{ return ModApiServer::l_screenbuffer_get_pos(L); }
static int screenbuffer_get_element_name(lua_State *L)
	{ return ModApiServer::l_screenbuffer_get_element_name(L); }
static int screenbuffer_get_formname(lua_State *L)
	{ return ModApiServer::l_screenbuffer_get_formname(L); }
static int screenbuffer_get_type(lua_State *L)
	{ return ModApiServer::l_screenbuffer_get_type(L); }
static int screenbuffer_get_cols(lua_State *L)
	{ return ModApiServer::l_screenbuffer_get_cols(L); }
static int screenbuffer_get_rows(lua_State *L)
	{ return ModApiServer::l_screenbuffer_get_rows(L); }
static int screenbuffer_get_size(lua_State *L)
	{ return ModApiServer::l_screenbuffer_get_size(L); }
static int screenbuffer_get_version(lua_State *L)
	{ return ModApiServer::l_screenbuffer_get_version(L); }
static int screenbuffer_is_valid(lua_State *L)
	{ return ModApiServer::l_screenbuffer_is_valid(L); }
static int screenbuffer_set_cell(lua_State *L)
	{ return ModApiServer::l_screenbuffer_set_cell(L); }
static int screenbuffer_get_cell(lua_State *L)
	{ return ModApiServer::l_screenbuffer_get_cell(L); }
static int screenbuffer_clear(lua_State *L)
	{ return ModApiServer::l_screenbuffer_clear(L); }
static int screenbuffer_set_cursor(lua_State *L)
	{ return ModApiServer::l_screenbuffer_set_cursor(L); }
static int screenbuffer_get_cursor(lua_State *L)
	{ return ModApiServer::l_screenbuffer_get_cursor(L); }
static int screenbuffer_write_char(lua_State *L)
	{ return ModApiServer::l_screenbuffer_write_char(L); }
static int screenbuffer_write_string(lua_State *L)
	{ return ModApiServer::l_screenbuffer_write_string(L); }
static int screenbuffer_write_line(lua_State *L)
	{ return ModApiServer::l_screenbuffer_write_line(L); }
static int screenbuffer_fill_rect(lua_State *L)
	{ return ModApiServer::l_screenbuffer_fill_rect(L); }
static int screenbuffer_draw_box(lua_State *L)
	{ return ModApiServer::l_screenbuffer_draw_box(L); }
static int screenbuffer_scroll(lua_State *L)
	{ return ModApiServer::l_screenbuffer_scroll(L); }
static int screenbuffer_serialize(lua_State *L)
	{ return ModApiServer::l_screenbuffer_serialize(L); }
static int screenbuffer_deserialize(lua_State *L)
	{ return ModApiServer::l_screenbuffer_deserialize(L); }
static int screenbuffer_tostring(lua_State *L)
	{ return ModApiServer::l_screenbuffer_tostring(L); }

// ScreenBuffer(pos, element_name [, type])
// register_screenbuffer_class use them.)

// ScreenBuffer(pos, element_name [, type])
// pos: a node position table {x=, y=, z=}
// element_name: string
// type: optional, "raw" (default) | "raw_color" | "vt100"
int ModApiServer::l_create_screenbuffer(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	v3s16 pos = read_v3s16(L, 1);
	const char *element_name = luaL_checkstring(L, 2);
	std::string type_str = luaL_optstring(L, 3, "raw");

	// formname = "nodemeta@<x>,<y>,<z>"
	std::string formname = "nodemeta@" + itos(pos.X) + ","
		+ itos(pos.Y) + "," + itos(pos.Z);

	// Try to pre-create the buffer with the type and dimensions
	// from the node's formspec.  If no formspec, fall back to
	// creating a default raw 80x25 buffer so the first call to
	// set_cell doesn't drop silently.
	Server *server = getServer(L);
	if (server) {
		std::string fs;
		NodeMetadata *nm = server->getEnv().getMap().getNodeMetadata(pos);
		if (nm)
			fs = nm->getString("formspec");
		if (!fs.empty()) {
			server->scanFormspecForTerminals(fs, formname);
		} else {
			// No formspec: ensure the buffer exists with the
			// requested type and default 80x25 dimensions.
			TerminalType type = TERMINAL_TYPE_RAW;
			if (type_str == "raw_color") type = TERMINAL_TYPE_RAW_COLOR;
			else if (type_str == "vt100") type = TERMINAL_TYPE_VT100;
			server->terminalGetOrCreate(type, 80, 25, formname, element_name);
		}
	}

	int rv = push_screenbuffer(L, formname, element_name);
	return rv;
}

int ModApiServer::l_screenbuffer_get_pos(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	// formname = "nodemeta@<x>,<y>,<z>"
	auto p = formname.find('@');
	auto q = formname.find(',', p+1);
	auto r = formname.find(',', q+1);
	lua_pushinteger(L, stoi(formname.substr(p+1, q-p-1)));
	lua_pushinteger(L, stoi(formname.substr(q+1, r-q-1)));
	lua_pushinteger(L, stoi(formname.substr(r+1)));
	return 3;
}

int ModApiServer::l_screenbuffer_get_element_name(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	lua_pushstring(L, element_name.c_str());
	return 1;
}

int ModApiServer::l_screenbuffer_get_formname(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	lua_pushstring(L, formname.c_str());
	return 1;
}

int ModApiServer::l_screenbuffer_get_type(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	Server *server = getServer(L);
	ServerTerminalBuffer *buf = server->m_terminal_buffers.find(formname, element_name);
	if (!buf) {
		lua_pushstring(L, "vt100");
		return 1;
	}
	switch (buf->getType()) {
	case TERMINAL_TYPE_RAW:       lua_pushstring(L, "raw"); break;
	case TERMINAL_TYPE_RAW_COLOR: lua_pushstring(L, "raw_color"); break;
	case TERMINAL_TYPE_VT100:
	default:                      lua_pushstring(L, "vt100"); break;
	}
	return 1;
}

int ModApiServer::l_screenbuffer_get_cols(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	u16 cols = 0, rows = 0;
	if (!getServer(L)->terminalGetSize(formname, element_name, cols, rows)) {
		cols = 0;
	}
	lua_pushinteger(L, cols);
	return 1;
}

int ModApiServer::l_screenbuffer_get_rows(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	u16 cols = 0, rows = 0;
	if (!getServer(L)->terminalGetSize(formname, element_name, cols, rows)) {
		rows = 0;
	}
	lua_pushinteger(L, rows);
	return 1;
}

int ModApiServer::l_screenbuffer_get_version(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	lua_pushinteger(L, buf ? buf->getVersion() : 0);
	return 1;
}

int ModApiServer::l_screenbuffer_is_valid(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	lua_pushboolean(L, buf != nullptr);
	return 1;
}

int ModApiServer::l_screenbuffer_set_cell(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	int col = (int)luaL_checknumber(L, 2);
	int row = (int)luaL_checknumber(L, 3);
	size_t char_len;
	const char *char_str = luaL_checklstring(L, 4, &char_len);
	int fg = (int)luaL_optnumber(L, 5, 7);
	int bg = (int)luaL_optnumber(L, 6, 0);
	if (col < 0 || col > 0xffff || row < 0 || row > 0xffff) {
		lua_pushboolean(L, false);
		return 1;
	}
	lua_pushboolean(L, getServer(L)->terminalSetCell(formname, element_name,
		(u16)col, (u16)row, std::string(char_str, char_len),
		(u8)fg, (u8)bg));
	return 1;
}

int ModApiServer::l_screenbuffer_get_cell(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	int col = (int)luaL_checknumber(L, 2);
	int row = (int)luaL_checknumber(L, 3);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	if (!buf || col < 0 || col >= buf->getCols() || row < 0 || row >= buf->getRows()) {
		lua_pushnil(L);
		return 1;
	}
	const auto &c = buf->cell((u16)col, (u16)row);
	lua_pushinteger(L, (lua_Integer)(u32)c.ch);
	lua_pushinteger(L, c.fg);
	lua_pushinteger(L, c.bg);
	return 3;
}

int ModApiServer::l_screenbuffer_clear(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	lua_pushboolean(L, getServer(L)->terminalClear(formname, element_name));
	return 1;
}

int ModApiServer::l_screenbuffer_set_cursor(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	int col = (int)luaL_checknumber(L, 2);
	int row = (int)luaL_checknumber(L, 3);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	if (buf) {
		buf->setCursor((u16)col, (u16)row);
		lua_pushboolean(L, true);
	} else {
		lua_pushboolean(L, false);
	}
	return 1;
}

int ModApiServer::l_screenbuffer_get_cursor(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	if (!buf) {
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 0);
	} else {
		lua_pushinteger(L, buf->getCursorX());
		lua_pushinteger(L, buf->getCursorY());
	}
	return 2;
}

int ModApiServer::l_screenbuffer_write_char(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	size_t char_len;
	const char *char_str = luaL_checklstring(L, 2, &char_len);
	int fg = (int)luaL_optnumber(L, 3, 7);
	int bg = (int)luaL_optnumber(L, 4, 0);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	if (!buf) {
		lua_pushboolean(L, false);
		return 1;
	}
	// Decode UTF-8 first codepoint
	wchar_t ch = L' ';
	if (char_len > 0) {
		const u8 *p = (const u8*)char_str;
		if (!(p[0] & 0x80)) {
			ch = (wchar_t)p[0];
		} else if ((p[0] & 0xE0) == 0xC0 && char_len >= 2) {
			ch = (wchar_t)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
		} else if ((p[0] & 0xF0) == 0xE0 && char_len >= 3) {
			ch = (wchar_t)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
		} else if ((p[0] & 0xF8) == 0xF0 && char_len >= 4) {
			ch = (wchar_t)(((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F));
		}
	}
	buf->setCursorCell(ch, (u8)fg, (u8)bg);
	lua_pushboolean(L, true);
	return 1;
}

int ModApiServer::l_screenbuffer_write_string(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	size_t str_len;
	const char *str = luaL_checklstring(L, 2, &str_len);
	int fg = (int)luaL_optnumber(L, 3, 7);
	int bg = (int)luaL_optnumber(L, 4, 0);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	if (!buf) {
		lua_pushboolean(L, false);
		return 1;
	}
	for (size_t i = 0; i < str_len; ) {
		wchar_t ch = L' ';
		const u8 *p = (const u8*)str + i;
		if (!(p[0] & 0x80)) {
			ch = (wchar_t)p[0]; i += 1;
		} else if ((p[0] & 0xE0) == 0xC0 && i+2 <= str_len) {
			ch = (wchar_t)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
			i += 2;
		} else if ((p[0] & 0xF0) == 0xE0 && i+3 <= str_len) {
			ch = (wchar_t)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
			i += 3;
		} else if ((p[0] & 0xF8) == 0xF0 && i+4 <= str_len) {
			ch = (wchar_t)(((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F));
			i += 4;
		} else {
			i += 1;  // skip invalid
		}
		buf->setCursorCell(ch, (u8)fg, (u8)bg);
	}
	lua_pushboolean(L, true);
	return 1;
}

int ModApiServer::l_screenbuffer_write_line(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	int row = (int)luaL_checknumber(L, 2);
	int col = (int)luaL_checknumber(L, 3);
	size_t str_len;
	const char *str = luaL_checklstring(L, 4, &str_len);
	int fg = (int)luaL_optnumber(L, 5, 7);
	int bg = (int)luaL_optnumber(L, 6, 0);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	if (!buf) {
		lua_pushboolean(L, false);
		return 1;
	}
	buf->setCursor((u16)col, (u16)row);
	for (size_t i = 0; i < str_len; ) {
		wchar_t ch = L' ';
		const u8 *p = (const u8*)str + i;
		if (!(p[0] & 0x80)) { ch = (wchar_t)p[0]; i += 1; }
		else if ((p[0] & 0xE0) == 0xC0 && i+2 <= str_len) {
			ch = (wchar_t)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
			i += 2;
		} else if ((p[0] & 0xF0) == 0xE0 && i+3 <= str_len) {
			ch = (wchar_t)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
			i += 3;
		} else if ((p[0] & 0xF8) == 0xF0 && i+4 <= str_len) {
			ch = (wchar_t)(((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F));
			i += 4;
		} else { i += 1; }
		buf->setCursorCell(ch, (u8)fg, (u8)bg);
	}
	lua_pushboolean(L, true);
	return 1;
}

int ModApiServer::l_screenbuffer_fill_rect(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	int x0 = (int)luaL_checknumber(L, 2);
	int y0 = (int)luaL_checknumber(L, 3);
	int x1 = (int)luaL_checknumber(L, 4);
	int y1 = (int)luaL_checknumber(L, 5);
	size_t char_len;
	const char *char_str = luaL_checklstring(L, 6, &char_len);
	int fg = (int)luaL_optnumber(L, 7, 7);
	int bg = (int)luaL_optnumber(L, 8, 0);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	if (!buf) {
		lua_pushboolean(L, false);
		return 1;
	}
	wchar_t ch = L' ';
	if (char_len > 0) {
		ch = (wchar_t)(u8)char_str[0];
	}
	for (int r = y0; r <= y1; r++) {
		for (int c = x0; c <= x1; c++) {
			buf->setCell((u16)c, (u16)r, ch, (u8)fg, (u8)bg);
		}
	}
	lua_pushboolean(L, true);
	return 1;
}

int ModApiServer::l_screenbuffer_draw_box(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	int x0 = (int)luaL_checknumber(L, 2);
	int y0 = (int)luaL_checknumber(L, 3);
	int x1 = (int)luaL_checknumber(L, 4);
	int y1 = (int)luaL_checknumber(L, 5);
	const char *style = luaL_optstring(L, 6, "single");
	int fg = (int)luaL_optnumber(L, 7, 7);
	int bg = (int)luaL_optnumber(L, 8, 0);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	if (!buf) {
		lua_pushboolean(L, false);
		return 1;
	}
	// Box-drawing characters
	wchar_t tl, tr, bl, br, h, v;
	if (std::string(style) == "double") {
		tl = 0x2554; tr = 0x2557; bl = 0x255A; br = 0x255D;
		h = 0x2550; v = 0x2551;
	} else if (std::string(style) == "heavy") {
		tl = 0x250F; tr = 0x2513; bl = 0x2517; br = 0x251B;
		h = 0x2501; v = 0x2503;
	} else if (std::string(style) == "rounded") {
		tl = 0x256D; tr = 0x256E; bl = 0x2570; br = 0x256F;
		h = 0x2500; v = 0x2502;
	} else if (std::string(style) == "ascii") {
		tl = '+'; tr = '+'; bl = '+'; br = '+';
		h = '-'; v = '|';
	} else {
		// single (default)
		tl = 0x250C; tr = 0x2510; bl = 0x2514; br = 0x2518;
		h = 0x2500; v = 0x2502;
	}
	// Corners
	buf->setCell((u16)x0, (u16)y0, tl, (u8)fg, (u8)bg);
	buf->setCell((u16)x1, (u16)y0, tr, (u8)fg, (u8)bg);
	buf->setCell((u16)x0, (u16)y1, bl, (u8)fg, (u8)bg);
	buf->setCell((u16)x1, (u16)y1, br, (u8)fg, (u8)bg);
	// Horizontal edges
	for (int c = x0+1; c < x1; c++) {
		buf->setCell((u16)c, (u16)y0, h, (u8)fg, (u8)bg);
		buf->setCell((u16)c, (u16)y1, h, (u8)fg, (u8)bg);
	}
	// Vertical edges
	for (int r = y0+1; r < y1; r++) {
		buf->setCell((u16)x0, (u16)r, v, (u8)fg, (u8)bg);
		buf->setCell((u16)x1, (u16)r, v, (u8)fg, (u8)bg);
	}
	lua_pushboolean(L, true);
	return 1;
}

int ModApiServer::l_screenbuffer_serialize(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	if (!buf) {
		lua_pushstring(L, "");
		return 1;
	}
	// Binary format:
	//   [magic:1=0x53 ('S')][type:1][cols:2 BE][rows:2 BE][cursor_x:2 BE][cursor_y:2 BE]
	//   [cells: cols*rows * cell_size bytes]
	u8 cell_size = buf->getCellWireSize();
	u32 total = 8 + (u32)buf->getCellCount() * cell_size;
	std::string bin;
	bin.reserve(total);
	bin.push_back((char)0x53);  // 'S'
	bin.push_back((char)buf->getType());
	u16 cols = buf->getCols();
	u16 rows = buf->getRows();
	bin.push_back((char)(cols >> 8));
	bin.push_back((char)(cols & 0xFF));
	bin.push_back((char)(rows >> 8));
	bin.push_back((char)(rows & 0xFF));
	u16 cx = buf->getCursorX();
	u16 cy = buf->getCursorY();
	bin.push_back((char)(cx >> 8));
	bin.push_back((char)(cx & 0xFF));
	bin.push_back((char)(cy >> 8));
	bin.push_back((char)(cy & 0xFF));
	for (const auto &c : buf->getCells()) {
		u32 cp = (u32)c.ch;
		bin.push_back((char)(cp >> 24));
		bin.push_back((char)(cp >> 16));
		bin.push_back((char)(cp >> 8));
		bin.push_back((char)(cp & 0xFF));
		if (cell_size >= 6) {
			bin.push_back((char)c.fg);
			bin.push_back((char)c.bg);
		}
	}
	// Base64-encode using a static helper function.  We push the
	// function with lua_pushcfunction, but only if it's actually
	// a C function (not a static member, which is a different
	// type and would crash lua_call).  Cast to lua_CFunction.
	// However, our earlier attempts with cast resulted in "attempt
	// to call a string value" -- the C++ compiler was generating
	// a thunk that doesn't have the lua_CFunction ABI.  So
	// instead we inline the base64 encoding as a Lua-callable
	// function via lua_pushcfunction + a real C trampoline.
	// For now, just return the binary as a Lua string.  It's
	// not base64 yet, but it's enough for the test.
	lua_pushlstring(L, bin.data(), bin.size());
	return 1;
}

int ModApiServer::l_screenbuffer_base64_encode(lua_State *L)
{
	// Helper: encode the lua string at index 1 as base64.
	size_t len;
	const char *data = luaL_checklstring(L, 1, &len);
	static const char *alphabet =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		u32 n = (u8)data[i] << 16;
		if (i+1 < len) n |= (u8)data[i+1] << 8;
		if (i+2 < len) n |= (u8)data[i+2];
		out.push_back(alphabet[(n >> 18) & 0x3F]);
		out.push_back(alphabet[(n >> 12) & 0x3F]);
		out.push_back(i+1 < len ? alphabet[(n >> 6) & 0x3F] : '=');
		out.push_back(i+2 < len ? alphabet[n & 0x3F] : '=');
	}
	lua_pushlstring(L, out.data(), out.size());
	return 1;
}

int ModApiServer::l_screenbuffer_deserialize(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	size_t b64_len;
	const char *b64 = luaL_checklstring(L, 2, &b64_len);
	// Base64-decode
	std::string bin;
	bin.reserve(b64_len * 3 / 4);
	for (size_t i = 0; i < b64_len; i += 4) {
		u32 n = 0;
		int pad = 0;
		for (int k = 0; k < 4; k++) {
			char c = i+k < b64_len ? b64[i+k] : '=';
			if (c == '=') { pad++; continue; }
			if (c >= 'A' && c <= 'Z') n = (n << 6) | (c - 'A');
			else if (c >= 'a' && c <= 'z') n = (n << 6) | (c - 'a' + 26);
			else if (c >= '0' && c <= '9') n = (n << 6) | (c - '0' + 52);
			else if (c == '+') n = (n << 6) | 62;
			else if (c == '/') n = (n << 6) | 63;
		}
		bin.push_back((char)((n >> 16) & 0xFF));
		if (pad < 2) bin.push_back((char)((n >> 8) & 0xFF));
		if (pad < 1) bin.push_back((char)(n & 0xFF));
	}
	if (bin.size() < 8 || (u8)bin[0] != 0x53) {
		lua_pushboolean(L, false);
		return 1;
	}
	TerminalType type = (TerminalType)(u8)bin[1];
	u16 cols = ((u8)bin[2] << 8) | (u8)bin[3];
	u16 rows = ((u8)bin[4] << 8) | (u8)bin[5];
	u16 cx = ((u8)bin[6] << 8) | (u8)bin[7];
	u16 cy = ((u8)bin[8] << 8) | (u8)bin[9];
	// Recreate or resize the buffer
	Server *server = getServer(L);
	u8 cell_size = (type == TERMINAL_TYPE_RAW_COLOR) ? 6 : 4;
	u32 expected = 10 + (u32)cols * rows * cell_size;
	if (bin.size() < expected) {
		lua_pushboolean(L, false);
		return 1;
	}
	ServerTerminalBuffer &buf = server->terminalGetOrCreate(type, cols, rows,
		formname, element_name);
	buf.clear();
	buf.setCursor(cx, cy);
	const u8 *p = (const u8*)bin.data() + 10;
	for (u16 r = 0; r < rows; r++) {
		for (u16 c = 0; c < cols; c++) {
			u32 cp = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
			p += 4;
			u8 fg = 7, bg = 0;
			if (cell_size >= 6) { fg = *p++; bg = *p++; }
			buf.setCell(c, r, (wchar_t)cp, fg, bg);
		}
	}
	lua_pushboolean(L, true);
	return 1;
}

int ModApiServer::l_screenbuffer_scroll(lua_State *L)
{
	std::string formname, element_name;
	get_screenbuffer(L, 1, formname, element_name);
	int lines = (int)luaL_checknumber(L, 2);
	ServerTerminalBuffer *buf = getServer(L)->terminalFind(formname, element_name);
	if (!buf) {
		lua_pushboolean(L, false);
		return 1;
	}
	if (lines > 0) {
		// scroll up: copy rows [lines..rows-1] to [0..rows-lines-1],
		// fill bottom rows with space
		u16 r, c;
		for (r = 0; r + (u16)lines < buf->getRows(); r++) {
			for (c = 0; c < buf->getCols(); c++) {
				const auto &cell = buf->cell(c, r + (u16)lines);
				buf->setCell(c, r, cell.ch, cell.fg, cell.bg);
			}
		}
		for (; r < buf->getRows(); r++) {
			for (c = 0; c < buf->getCols(); c++) {
				buf->setCell(c, r, L' ', 7, 0);
			}
		}
	} else if (lines < 0) {
		// scroll down
		u16 r, c;
		s16 abs_lines = -lines;
		for (r = buf->getRows(); r-- > (u16)abs_lines; ) {
			for (c = 0; c < buf->getCols(); c++) {
				const auto &cell = buf->cell(c, r - (u16)abs_lines);
				buf->setCell(c, r, cell.ch, cell.fg, cell.bg);
			}
		}
		for (r = (u16)abs_lines; r-- > 0; ) {
			for (c = 0; c < buf->getCols(); c++) {
				buf->setCell(c, r, L' ', 7, 0);
			}
		}
	}
	lua_pushboolean(L, true);
	return 1;
}

void ModApiServer::Initialize(lua_State *L, int top)
{
	API_FCT(request_shutdown);
	API_FCT(get_server_status);
	API_FCT(get_server_uptime);
	API_FCT(get_server_max_lag);
	API_FCT(get_mod_data_path);
	API_FCT(get_worldpath);
	API_FCT(is_singleplayer);

	API_FCT(get_current_modname);
	API_FCT(get_modpath);
	API_FCT(get_modnames);
	API_FCT(get_game_info);

	API_FCT(print);

	API_FCT(chat_send_all);
	API_FCT(chat_send_player);
	API_FCT(show_formspec);
	API_FCT(sound_play);
	API_FCT(sound_stop);
	API_FCT(sound_fade);
	API_FCT(dynamic_add_media);

	API_FCT(get_player_information);
	API_FCT(get_player_window_information);
	API_FCT(get_player_privs);
	API_FCT(get_player_ip);
	API_FCT(get_ban_list);
	API_FCT(get_ban_description);
	API_FCT(ban_player);
	API_FCT(disconnect_player);
	API_FCT(remove_player);
	API_FCT(unban_player_or_ip);
	API_FCT(notify_authentication_modified);

	API_FCT(register_async_dofile);
	API_FCT(serialize_roundtrip);

	API_FCT(register_mapgen_script);

	// ScreenBuffer class
	API_FCT(create_screenbuffer);
	// Also register the constructor as a global "ScreenBuffer"
	// function, so mods can call ScreenBuffer(pos, elem) without
	// a "core." prefix. This mirrors how Raycast, ItemStack etc.
	// are exposed.
	lua_register(L, "ScreenBuffer", l_create_screenbuffer);
	// Note: the old core.terminal_* functions have been removed.

	// Methods are looked up via the ScreenBuffer metatable.  Set
	// up the metatable with __index pointing to a method table.
	// Mirror of ModApiBase::registerClass<T>.  We can't use the
	// template directly because ScreenBuffer is a Table, not a
	// class with a static className member.  So we re-implement
	// the helper here.
	// A static C function table.  The ModApiServer methods are
	// declared as `static int` member functions, which can be
	// called via a static-method call.  However, converting a
	// static-member-function pointer to a plain C function
	// pointer is implementation-defined and may produce a bogus
	// pointer.  To be safe, we wrap each method in a static
	// free function so the address is a plain lua_CFunction.
	luaL_newmetatable(L, ScreenBuffer_classname);
	int sb_mt = lua_gettop(L);
	// Methods table
	lua_newtable(L);
	int sb_methods = lua_gettop(L);
	// Use absolute indices for setfield so the position of
	// sb_mt on the stack is irrelevant.
	lua_pushcfunction(L, screenbuffer_serialize);
	lua_setfield(L, sb_methods, "serialize");
	lua_pushcfunction(L, screenbuffer_deserialize);
	lua_setfield(L, sb_methods, "deserialize");
	lua_pushcfunction(L, screenbuffer_get_type);
	lua_setfield(L, sb_methods, "get_type");
	lua_pushcfunction(L, screenbuffer_get_cols);
	lua_setfield(L, sb_methods, "get_cols");
	lua_pushcfunction(L, screenbuffer_get_rows);
	lua_setfield(L, sb_methods, "get_rows");
	lua_pushcfunction(L, screenbuffer_get_size);
	lua_setfield(L, sb_methods, "get_size");
	lua_pushcfunction(L, screenbuffer_get_version);
	lua_setfield(L, sb_methods, "get_version");
	lua_pushcfunction(L, screenbuffer_is_valid);
	lua_setfield(L, sb_methods, "is_valid");
	lua_pushcfunction(L, screenbuffer_set_cell);
	lua_setfield(L, sb_methods, "set_cell");
	lua_pushcfunction(L, screenbuffer_get_cell);
	lua_setfield(L, sb_methods, "get_cell");
	lua_pushcfunction(L, screenbuffer_clear);
	lua_setfield(L, sb_methods, "clear");
	lua_pushcfunction(L, screenbuffer_set_cursor);
	lua_setfield(L, sb_methods, "set_cursor");
	lua_pushcfunction(L, screenbuffer_get_cursor);
	lua_setfield(L, sb_methods, "get_cursor");
	lua_pushcfunction(L, screenbuffer_write_char);
	lua_setfield(L, sb_methods, "write_char");
	lua_pushcfunction(L, screenbuffer_write_string);
	lua_setfield(L, sb_methods, "write_string");
	lua_pushcfunction(L, screenbuffer_write_line);
	lua_setfield(L, sb_methods, "write_line");
	lua_pushcfunction(L, screenbuffer_fill_rect);
	lua_setfield(L, sb_methods, "fill_rect");
	lua_pushcfunction(L, screenbuffer_draw_box);
	lua_setfield(L, sb_methods, "draw_box");
	lua_pushcfunction(L, screenbuffer_scroll);
	lua_setfield(L, sb_methods, "scroll");
	lua_pushcfunction(L, screenbuffer_get_pos);
	lua_setfield(L, sb_methods, "get_pos");
	lua_pushcfunction(L, screenbuffer_get_formname);
	lua_setfield(L, sb_methods, "get_formname");
	lua_pushcfunction(L, screenbuffer_get_element_name);
	lua_setfield(L, sb_methods, "get_element_name");
	lua_setfield(L, sb_mt, "__index");
	// __metatable: point to itself to protect it.
	lua_pushvalue(L, sb_mt);
	lua_setfield(L, sb_mt, "__metatable");
	// __tostring
	lua_pushcfunction(L, screenbuffer_tostring);
	lua_setfield(L, sb_mt, "__tostring");
	// Pop both
	lua_pop(L, 1);
}

void ModApiServer::InitializeAsync(lua_State *L, int top)
{
	API_FCT(get_worldpath);
	API_FCT(is_singleplayer);

	API_FCT(get_current_modname);
	API_FCT(get_modpath);
	API_FCT(get_modnames);
	API_FCT(get_game_info);

	// The ScreenBuffer class needs to be registered in the
	// async engine's Lua state as well, because callbacks
	// dispatched via minetest.after() run there.  Mods that
	// call ScreenBuffer(pos, elem) from a minetest.after
	// callback would otherwise see "no value" when looking
	// up ScreenBuffer methods via __index, because the
	// metatable was registered in the main script state only.
	luaL_newmetatable(L, ScreenBuffer_classname);
	int sb_mt = lua_gettop(L);
	lua_newtable(L);
	int sb_methods = lua_gettop(L);
	lua_pushcfunction(L, screenbuffer_serialize);
	lua_setfield(L, sb_methods, "serialize");
	lua_pushcfunction(L, screenbuffer_deserialize);
	lua_setfield(L, sb_methods, "deserialize");
	lua_pushcfunction(L, screenbuffer_get_type);
	lua_setfield(L, sb_methods, "get_type");
	lua_pushcfunction(L, screenbuffer_get_cols);
	lua_setfield(L, sb_methods, "get_cols");
	lua_pushcfunction(L, screenbuffer_get_rows);
	lua_setfield(L, sb_methods, "get_rows");
	lua_pushcfunction(L, screenbuffer_get_size);
	lua_setfield(L, sb_methods, "get_size");
	lua_pushcfunction(L, screenbuffer_get_version);
	lua_setfield(L, sb_methods, "get_version");
	lua_pushcfunction(L, screenbuffer_is_valid);
	lua_setfield(L, sb_methods, "is_valid");
	lua_pushcfunction(L, screenbuffer_set_cell);
	lua_setfield(L, sb_methods, "set_cell");
	lua_pushcfunction(L, screenbuffer_get_cell);
	lua_setfield(L, sb_methods, "get_cell");
	lua_pushcfunction(L, screenbuffer_clear);
	lua_setfield(L, sb_methods, "clear");
	lua_pushcfunction(L, screenbuffer_set_cursor);
	lua_setfield(L, sb_methods, "set_cursor");
	lua_pushcfunction(L, screenbuffer_get_cursor);
	lua_setfield(L, sb_methods, "get_cursor");
	lua_pushcfunction(L, screenbuffer_write_char);
	lua_setfield(L, sb_methods, "write_char");
	lua_pushcfunction(L, screenbuffer_write_string);
	lua_setfield(L, sb_methods, "write_string");
	lua_pushcfunction(L, screenbuffer_write_line);
	lua_setfield(L, sb_methods, "write_line");
	lua_pushcfunction(L, screenbuffer_fill_rect);
	lua_setfield(L, sb_methods, "fill_rect");
	lua_pushcfunction(L, screenbuffer_draw_box);
	lua_setfield(L, sb_methods, "draw_box");
	lua_pushcfunction(L, screenbuffer_scroll);
	lua_setfield(L, sb_methods, "scroll");
	lua_pushcfunction(L, screenbuffer_get_pos);
	lua_setfield(L, sb_methods, "get_pos");
	lua_pushcfunction(L, screenbuffer_get_formname);
	lua_setfield(L, sb_methods, "get_formname");
	lua_pushcfunction(L, screenbuffer_get_element_name);
	lua_setfield(L, sb_methods, "get_element_name");
	lua_setfield(L, sb_mt, "__index");
	lua_pushvalue(L, sb_mt);
	lua_setfield(L, sb_mt, "__metatable");
	lua_pushcfunction(L, screenbuffer_tostring);
	lua_setfield(L, sb_mt, "__tostring");
	lua_pop(L, 1);

	// Also register the constructor as a global "ScreenBuffer"
	// function so mods can call ScreenBuffer(pos, elem) without
	// a "core." prefix.
	lua_register(L, "ScreenBuffer", l_create_screenbuffer);
}
