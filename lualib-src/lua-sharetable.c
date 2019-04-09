#define LUA_LIB

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "lstring.h"
#include "lobject.h"
#include "ltable.h"
#include "lstate.h"
#include "lapi.h"

#ifdef ENABLE_SHORT_STRING_TABLE

#define MAX_LEVEL 128

static int
readerror(lua_State *L) {
	return luaL_error(L, "The table is readonly");
}

static int
raw_next(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_settop(L, 2);
	if (lua_next(L, 1))
		return 2;
	else {
		lua_pushnil(L);
		return 1;
	}
}

static int
pairs_proxy(lua_State *L) {
	lua_pushcfunction(L, raw_next);
	if (luaL_getmetafield(L, 1, "__index") != LUA_TTABLE) {
		luaL_error(L, "Invalid readonly table");
	}
	lua_pushnil(L);
	return 3;
}

static void
readonly(lua_State *L, int deep) {
	if (deep > MAX_LEVEL) {
		luaL_error(L, "Table is too deep");
	}
	if (lua_type(L, -1) != LUA_TTABLE) {
		return;
	}
	luaL_checkstack(L, 4, NULL);
	lua_newtable(L);
	lua_createtable(L, 0, 4);
	lua_pushvalue(L, -3);
	// tbl, proxy, meta, tbl
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, readerror);
	lua_setfield(L, -2, "__newindex");
	lua_pushcfunction(L, pairs_proxy);
	lua_setfield(L, -2, "__pairs");
	lua_pushstring(L, "readonly");
	lua_setfield(L, -2, "__metatable");
	lua_setmetatable(L, -2);
	// tbl, proxy (with meta) 

	lua_pushnil(L);
	while (lua_next(L, -3) != 0) {
		if (lua_type(L, -1) == LUA_TTABLE) {
			readonly(L, deep+1);
			// tbl, tbl_proxy, key, value_proxy
			lua_pushvalue(L, -2);
			lua_insert(L, -2);
			// tbl, tbl_proxy, key, key, value_proxy
			lua_rawset(L, -5);
			// tbl, tbl_proxy, key
		} else {
			lua_pop(L, 1);
		}
		if (lua_type(L, -1) == LUA_TTABLE) {
			luaL_error(L, "Key must not be a table");
		}
	}
	// tbl, proxy
	lua_replace(L, -2);
	// proxy
}

static int
make_readonly(lua_State *L) {
	readonly(L, 0);
	return 1;
}

static void
checkshared_shortstring(lua_State *L, const char * str) {
	lua_pushstring(L, str);
	TString * s = tsvalue(L->top-1);
	if (luaS_clonestring(L, s) != s) {
		luaL_error(L, "%s is not a shared string", str);
	}
	lua_pop(L, 1);
}

static void
checkshared(lua_State *L) {
	checkshared_shortstring(L, "__index");
	checkshared_shortstring(L, "__newindex");
	checkshared_shortstring(L, "__metatable");
	checkshared_shortstring(L, "__pairs");
	checkshared_shortstring(L, "readonly");
}

static void
mark_shared(lua_State *L, int deep) {
	if (deep > MAX_LEVEL) {
		luaL_error(L, "Table is too deep");
	}
	if (lua_type(L, -1) != LUA_TTABLE) {
		luaL_error(L, "Not a table, it's a %s.", lua_typename(L, lua_type(L, -1)));
	}
	luaL_checkstack(L, 4, NULL);
	if (lua_getmetatable(L, -1)) {
		mark_shared(L, deep + 1);
		lua_pop(L, 1);
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		int vt = lua_type(L, -1);
		switch (vt) {
		case LUA_TTABLE:
			mark_shared(L, deep + 1);
			break;
		case LUA_TNUMBER:
		case LUA_TBOOLEAN:
		case LUA_TSTRING:
		case LUA_TLIGHTUSERDATA:
			break;
		case LUA_TFUNCTION:
			if (!lua_iscfunction(L, -1) || lua_getupvalue(L, -1, 1) != NULL)
				luaL_error(L, "Invalid value function");
			break;
		default:
			luaL_error(L, "Invalid value type [%s]", lua_typename(L, vt));
			break;
		}
		lua_pop(L, 1);
		int kt = lua_type(L, -1);
		switch (kt) {
		case LUA_TNUMBER:
		case LUA_TBOOLEAN:
		case LUA_TSTRING:
		case LUA_TLIGHTUSERDATA:
			break;
		default:
			luaL_error(L, "Invalid key type %s", lua_typename(L, kt));
			break;
		}
	}
	makeShared((Table *)lua_topointer(L, -1));
}

static int
make_matrix(lua_State *L) {
	checkshared(L);
	// turn off gc , because marking shared will prevent gc mark.
	lua_gc(L, LUA_GCSTOP, 0);
	mark_shared(L, 0);
	Table * t = (Table *)lua_topointer(L, -1);
	lua_pushlightuserdata(L, t);
	return 1;
}

static int
valuepatch_next(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	if (luaL_getmetafield(L, 1, "__index") != LUA_TTABLE) {
		return luaL_error(L, "Invalid value patch table");
	}
	lua_pushvalue(L, 2);
	if (lua_next(L, -2)) {
		// key, value
		lua_pushvalue(L, -2);
		if (lua_rawget(L, 1) != LUA_TNIL) {
			// key, value, patchvalue
			lua_replace(L, -2);
		} else {
			lua_pop(L, 1);
		}
		return 2;
	} else {
		lua_pushnil(L);
		return 1;
	}
}

static int
pairs_valuepatch(lua_State *L) {
	lua_pushcfunction(L, valuepatch_next);
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	return 3;
}

static Table *
real_table(lua_State *L, Table *t) {
	sethvalue(L, L->top, t);
	api_incr_top(L);
	if (luaL_getmetafield(L, -1, "__index") != LUA_TTABLE) {
		luaL_error(L, "Invalid shared table");
	}
	Table * rt = hvalue(L->top-1);
	lua_pop(L, 2);
	return rt;
}

static void
patch_table(lua_State *L) {
	switch(lua_type(L, -3)) {
	case LUA_TNIL:
		lua_newtable(L);
		lua_replace(L, -4);
		break;
	case LUA_TTABLE:
		break;
	default:
		// never be here
		luaL_error(L, "Invalid patch table %s", lua_typename(L, lua_type(L, -3)));
		break;
	}
	lua_rawset(L, -3);
}

static TString *
clone_shared_string(lua_State *L, const TValue *v) {
	TString * s = tsvalue(v);
	TString * sc = luaS_clonestring(L, s);
	if (sc != s)
		return sc;
	return NULL;
}

static int
pairs_keypatch(lua_State *L) {
	lua_settop(L, 1);
	if (!lua_getmetatable(L, 1)) {
		luaL_error(L, "Invalid key patched table : no metatable");
	}
	if (lua_getfield(L, 2, "__index") != LUA_TTABLE) {
		luaL_error(L, "Invalid key patched table : no __index");
	}
	// patched(1)  metatable(2)  sharedtable(3)
	Table * t = (Table *)lua_topointer(L, 3);
	// copy array part
	unsigned int i;
	for (i=0;i<t->sizearray;i++) {
		if (lua_rawgeti(L, 1, i+1) == LUA_TNIL) {
			lua_pop(L, 1);
			TValue * v = &t->array[i];
			setobj(L, L->top, v);
			api_incr_top(L);
			lua_rawseti(L, 1, i+1);
		}
	}
	// copy hash part
	unsigned int hsz = sizenode(t);
	for (i=0;i<hsz;i++) {
		if (!ttisnil(gval(gnode(t, i)))) {
			const TValue *k = gkey(gnode(t, i));
			if (!(ttisshrstring(k) && clone_shared_string(L, k))) {
				// the key maybe need copy to pathed table
				setobj(L, L->top, k);
				api_incr_top(L);
				lua_pushvalue(L, -1);
				if (lua_gettable(L, 1) == LUA_TNIL) {
					luaL_error(L, "Invalid key patched table : no value");
				}
				lua_rawset(L, 1);
			}
		}
	}
	// All key/value pairs are in the patched table, clear __index and __pairs.
	lua_pushnil(L);
	lua_setfield(L, 2, "__index");
	lua_pushnil(L);
	lua_setfield(L, 2, "__pairs");
	lua_settop(L, 1);
	lua_pushcfunction(L, raw_next);
	lua_insert(L, -2);
	lua_pushnil(L);
	return 3;
}

static int clone(lua_State *L, Table *t);

static int
clone_value(lua_State *L, const TValue *v) {
	if (ttistable(v)) {
		if (clone(L, hvalue(v))) {
			return 1;
		}
	} else if (ttisshrstring(v)) {
		TString * s = clone_shared_string(L, v);
		if (s) {
			setsvalue(L, L->top, s);
			api_incr_top(L);
			return 1;
		}
	}
	return 0;
}

static int
clone(lua_State *L, Table *t) {
	luaL_checkstack(L, 4, NULL);
	int keypatch = 0;
	int valuepatch = 0;
	t = real_table(L, t);
	lua_pushnil(L);

	unsigned int i;
	for (i=0;i<t->sizearray;i++) {
		TValue * v = &t->array[i];
		if (clone_value(L, v)) {
			valuepatch = 1;
			lua_pushinteger(L, i+1);
			lua_insert(L, -2);
			patch_table(L);
		}
	}
	unsigned int hsz = sizenode(t);
	for (i=0;i<hsz;i++) {
		if (!ttisnil(gval(gnode(t, i)))) {
			const TValue *k = gkey(gnode(t, i));
			const TValue *v = gval(gnode(t, i));
			TString * keystr = NULL;
			if (ttisshrstring(k)) {
				keystr = clone_shared_string(L, k);
			}

			if (keystr) {
				keypatch = 1;
				setsvalue(L, L->top, keystr);
				api_incr_top(L);

				if (!clone_value(L, v)) {
					setobj(L,L->top, v);
					api_incr_top(L);
				}
				patch_table(L);
			} else if (clone_value(L, v)) {
				valuepatch = 1;
				setobj(L,L->top, k);
				api_incr_top(L);

				lua_insert(L, -2);
				patch_table(L);
			}
		}
	}

	if (keypatch || valuepatch) {
		lua_createtable(L, 0, 3);
		sethvalue(L, L->top, t);
		api_incr_top(L);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, readerror);
		lua_setfield(L, -2, "__newindex");
		if (keypatch) {
			lua_pushcfunction(L, pairs_keypatch);
		} else {
			lua_pushcfunction(L, pairs_valuepatch);
		}
		lua_setfield(L, -2, "__pairs");
		lua_setmetatable(L, -2);
		return 1;
	} else {
		// pop nil
		lua_pop(L, 1);
		return 0;
	}
}

static int
clone_table(lua_State *L) {
	checkshared(L);
	Table * t = (Table *)lua_touserdata(L, 1);
	if (!isShared(t))
		return luaL_error(L, "Not a shared table");

	if (!clone(L, t)) {
		sethvalue(L, L->top, t);
		api_incr_top(L);
	}

	return 1;
}

struct state_ud {
	lua_State *L;
};

static int
close_state(lua_State *L) {
	struct state_ud *ud = (struct state_ud *)luaL_checkudata(L, 1, "BOXMATRIXSTATE");
	if (ud->L) {
		lua_close(ud->L);
		ud->L = NULL;
	}
	return 0;
}

static int
get_matrix(lua_State *L) {
	struct state_ud *ud = (struct state_ud *)luaL_checkudata(L, 1, "BOXMATRIXSTATE");
	if (ud->L) {
		const void * v = lua_topointer(ud->L, 1);
		lua_pushlightuserdata(L, (void *)v);
		return 1;
	}
	return 0;
}

static int
get_size(lua_State *L) {
	struct state_ud *ud = (struct state_ud *)luaL_checkudata(L, 1, "BOXMATRIXSTATE");
	if (ud->L) {
		lua_Integer sz = lua_gc(L, LUA_GCCOUNT, 0);
		sz *= 1024;
		sz += lua_gc(L, LUA_GCCOUNTB, 0);
		lua_pushinteger(L, sz);
	} else {
		lua_pushinteger(L, 0);
	}
	return 1;
}


static int
box_state(lua_State *L, lua_State *mL) {
	struct state_ud *ud = (struct state_ud *)lua_newuserdata(L, sizeof(*ud));
	ud->L = mL;
	if (luaL_newmetatable(L, "BOXMATRIXSTATE")) {
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, close_state);
		lua_setfield(L, -2, "close");
		lua_pushcfunction(L, get_matrix);
		lua_setfield(L, -2, "getptr");
		lua_pushcfunction(L, get_size);
		lua_setfield(L, -2, "size");
	}
	lua_setmetatable(L, -2);

	return 1;
}

static int
load_matrixfile(lua_State *L) {
	luaL_openlibs(L);
	const char * source = (const char *)lua_touserdata(L, 1);
	if (source[0] == '@') {
		if (luaL_loadfilex_(L, source+1, NULL) || lua_pcall(L, 0, LUA_MULTRET, 0)) {
			lua_error(L);
		}
	} else {
		if (luaL_dostring(L, source) != LUA_OK) {
			lua_error(L);
		}
	}
	if (lua_gettop(L) == 0) {
		luaL_error(L, "No table returns");
	}
	lua_replace(L, 1);
	lua_settop(L, 1);
	lua_pushcfunction(L, make_readonly);
	lua_insert(L, -2);
	lua_call(L, 1, 1);
	lua_gc(L, LUA_GCCOLLECT, 0);
	lua_pushcfunction(L, make_matrix);
	lua_insert(L, -2);
	lua_call(L, 1, 1);
	return 1;
}

static int
matrix_from_file(lua_State *L) {
	luaS_expandshr(4096);
	lua_State *mL = luaL_newstate();
	if (mL == NULL) {
		luaS_expandshr(-4096);
		return luaL_error(L, "luaL_newstate failed");
	}
	const char * source = luaL_checkstring(L, 1);
	lua_pushcfunction(mL, load_matrixfile);
	lua_pushlightuserdata(mL, (void *)source);
	int ok = lua_pcall(mL, 1, 1, 0);
	luaS_expandshr(-4096);
	if (ok != LUA_OK) {
		lua_pushstring(L, lua_tostring(mL, -1));
		lua_close(mL);
		lua_error(L);
	}
	return box_state(L, mL);
}

LUAMOD_API int
luaopen_skynet_sharetable_core(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "clone", clone_table },
		{ "matrix", matrix_from_file },
		{ NULL, NULL },
	};
	luaL_newlib(L, l);
	return 1;
}

#else

LUAMOD_API int
luaopen_skynet_sharetable_core(lua_State *L) {
	return luaL_error(L, "No share string table support");
}

#endif