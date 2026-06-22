/*
** $Id: linit.c $
** Initialization of libraries for lua.c and other clients
** See Copyright Notice in lua.h
*/


#define linit_c
#define LUA_LIB


#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "lualib.h"
#include "lauxlib.h"
#include "llimits.h"


/*
** Standard Libraries. (Must be listed in the same ORDER of their
** respective constants LUA_<libname>K.)
*/
static const luaL_Reg stdlibs[] = {
  {LUA_GNAME, luaopen_base},
  {LUA_LOADLIBNAME, luaopen_package},
  {LUA_COLIBNAME, luaopen_coroutine},
  {LUA_DBLIBNAME, luaopen_debug},
  {LUA_IOLIBNAME, luaopen_io},
  {LUA_MATHLIBNAME, luaopen_math},
  {LUA_OSLIBNAME, luaopen_os},
  {LUA_STRLIBNAME, luaopen_string},
  {LUA_TABLIBNAME, luaopen_table},
  {LUA_UTF8LIBNAME, luaopen_utf8},
  {NULL, NULL}
};

static const luaL_Reg extlibs[] = {
  {LUA_LFS, luaopen_lfs},
  {LUA_CJSONLIBNAME, luaopen_cjson},
  {LUA_PBPACKNAME, luaopen_pb},
  {LUA_SREYTASK, luaopen_task},
  {LUA_SREYCORE, luaopen_core},
  {LUA_SREYHARBOR, luaopen_harbor},
  {LUA_SREYDATACENTER, luaopen_datacenter},
  {LUA_SREYSUBCENTER, luaopen_subcenter},
  {LUA_SREYDNS, luaopen_dns},
  {LUA_SREYCUSTZ, luaopen_custz},
  {LUA_SREYWEBSOCK, luaopen_websock},
  {LUA_SREYHTTP, luaopen_http},
  {LUA_SREYREDIS, luaopen_redis},
  {LUA_SMTP, luaopen_smtp},
  {LUA_SMTP_MAIL, luaopen_mail},
  {LUA_SREYUTILS, luaopen_utils},
  {LUA_SREYHASHRING, luaopen_hashring},
  {LUA_SREYTREND, luaopen_trend},
  {LUA_SREYPOPEN, luaopen_popen},
  {LUA_SREYSTM, luaopen_stm},
  {LUA_SREYURL, luaopen_url},
  {LUA_SREYBASE64, luaopen_base64},
  {LUA_SREYCRC,  luaopen_crc},
  {LUA_SREYDIGEST, luaopen_digest},
  {LUA_SREYHMAC, luaopen_hmac},
  {LUA_SREYCIPHER, luaopen_cipher},
  {LUA_SREYSERI, luaopen_seri},
  {LUA_MYSQL, luaopen_mysql},
  {LUA_MYSQLBIND, luaopen_mysql_bind},
  {LUA_MYSQLREADER, luaopen_mysql_reader},
  {LUA_MYSQLSTMT, luaopen_mysql_stmt},
  {LUA_PGSQL, luaopen_pgsql},
  {LUA_PGSQLBIND, luaopen_pgsql_bind},
  {LUA_PGSQLREADER, luaopen_pgsql_reader},
  {LUA_MQTT, luaopen_mqtt},
  {LUA_BSON, luaopen_bson},
  {LUA_BSONITER, luaopen_bson_iter},
  {LUA_MONGO, luaopen_mongo},
  {LUA_MONGOSESSION, luaopen_mongo_session},
  {LUA_SREYROUTER, luaopen_router},
  {NULL, NULL}
};

/*
** require and preload selected standard libraries
*/
LUALIB_API void luaL_openselectedlibs (lua_State *L, int load, int preload) {
  int mask;
  const luaL_Reg *lib;
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  for (lib = stdlibs, mask = 1; lib->name != NULL; lib++, mask <<= 1) {
    if (load & mask) {  /* selected? */
      luaL_requiref(L, lib->name, lib->func, 1);  /* require library */
      lua_pop(L, 1);  /* remove result from the stack */
    }
    else if (preload & mask) {  /* selected? */
      lua_pushcfunction(L, lib->func);
      lua_setfield(L, -2, lib->name);  /* add library to PRELOAD table */
    }
  }
  lua_assert((mask >> 1) == LUA_UTF8LIBK);
  for (lib = extlibs; lib->name != NULL; lib++) {
    luaL_requiref(L, lib->name, lib->func, 1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);  /* remove PRELOAD table */
}

