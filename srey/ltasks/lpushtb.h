#ifndef LPUSHTB_H_
#define LPUSHTB_H_

#if WITH_LUA
#define LUA_TBPUSH_NUMBER(name, val)\
lua_pushstring(lua, name);\
lua_pushinteger(lua, val);\
lua_settable(lua, -3)
#define LUA_TBPUSH_STRING(name, val)\
lua_pushstring(lua, name);\
lua_pushstring(lua, val);\
lua_settable(lua, -3)
#define LUA_TBPUSH_LSTRING(name, val, lens)\
lua_pushstring(lua, name);\
lua_pushlstring(lua, val, lens);\
lua_settable(lua, -3)
#define LUA_TBPUSH_UD(val, lens)\
lua_pushstring(lua, "data");\
lua_pushlightuserdata(lua, val);\
lua_settable(lua, -3);\
lua_pushstring(lua, "size");\
lua_pushinteger(lua, lens);\
lua_settable(lua, -3)
#endif

#endif//LPUSHTB_H_
