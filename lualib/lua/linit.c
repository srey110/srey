/*
** $Id: linit.c $
** Initialization of libraries for lua.c and other clients
** See Copyright Notice in lua.h
*/


#define linit_c
#define LUA_LIB

/*
** If you embed Lua in your program and need to open the standard
** libraries, call luaL_openlibs in your program. If you need a
** different set of libraries, copy this file to your project and edit
** it to suit your needs.
**
** You can also *preload* libraries, so that a later 'require' can
** open the library, which is already linked to the application.
** For that, do the following code:
**
**  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
**  lua_pushcfunction(L, luaopen_modname);
**  lua_setfield(L, -2, modname);
**  lua_pop(L, 1);  // remove PRELOAD table
*/

#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "lualib.h"
#include "lauxlib.h"


/*
** these libs are loaded by lua.c and are readily available to any Lua
** program
*/
static const luaL_Reg loadedlibs[] = {
  {LUA_GNAME, luaopen_base},
  {LUA_LOADLIBNAME, luaopen_package},
  {LUA_COLIBNAME, luaopen_coroutine},
  {LUA_TABLIBNAME, luaopen_table},
  {LUA_IOLIBNAME, luaopen_io},
  {LUA_OSLIBNAME, luaopen_os},
  {LUA_STRLIBNAME, luaopen_string},
  {LUA_MATHLIBNAME, luaopen_math},
  {LUA_UTF8LIBNAME, luaopen_utf8},
  {LUA_DBLIBNAME, luaopen_debug},

  {LUA_LFS, luaopen_lfs},
  {LUA_CJSONLIBNAME, luaopen_cjson},
  {LUA_PBPACKNAME, luaopen_pb},

  {LUA_SREYTASK, luaopen_task},
  {LUA_SREYCORE, luaopen_core},
  {LUA_SREYHARBOR, luaopen_harbor},
  {LUA_SREYDNS, luaopen_dns},
  {LUA_SREYCUSTZ, luaopen_custz},
  {LUA_SREYWEBSOCK, luaopen_websock},
  {LUA_SREYHTTP, luaopen_http},
  {LUA_SREYREDIS, luaopen_redis},
  {LUA_SMTP, luaopen_smtp},
  {LUA_SREYUTILS, luaopen_utils},
  {LUA_SREYHASHRING, luaopen_hashring},
  {LUA_SREYURL, luaopen_url},
  {LUA_SREYBASE64, luaopen_base64},
  {LUA_SREYCRC,  luaopen_crc},
  {LUA_SREYDIGEST, luaopen_digest},
  {LUA_SREYHMAC, luaopen_hmac},
  {LUA_SREYCIPHER, luaopen_cipher},
  {LUA_MYSQL, luaopen_mysql},
  {LUA_MYSQLBIND, luaopen_mysql_bind},
  {LUA_MYSQLREADER, luaopen_mysql_reader},
  {LUA_MYSQLSTMT, luaopen_mysql_stmt},

  {NULL, NULL}
};


LUALIB_API void luaL_openlibs (lua_State *L) {
  const luaL_Reg *lib;
  /* "require" functions from 'loadedlibs' and set results to global table */
  for (lib = loadedlibs; lib->func; lib++) {
    luaL_requiref(L, lib->name, lib->func, 1);
    lua_pop(L, 1);  /* remove lib */
  }
}

