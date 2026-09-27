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

/*
** srey 扩展模块：除 yyjson 外都进 PRELOAD，第一次 require 才打开。
** 某模块若要造"别的模块 luaopen 时注册的元表"的对象，须在自己的 luaopen 里
** luaL_requiref 那个模块，否则脚本没 require 它时对象拿不到方法
*/
static const luaL_Reg extlibs[] = {
  {LUA_SREY_TASK, luaopen_task},
  {LUA_SREY_CORE, luaopen_core},
  {LUA_SREY_HARBOR, luaopen_harbor},
  {LUA_SREY_DNS, luaopen_dns},
  {LUA_SREY_CUSTZ, luaopen_custz},
  {LUA_SREY_WEBSOCK, luaopen_websock},
  {LUA_SREY_HTTP, luaopen_http},
  {LUA_SREY_REDIS, luaopen_redis},
  {LUA_SREY_SMTP, luaopen_smtp},
  {LUA_SREY_SMTP_MAIL, luaopen_mail},
  {LUA_SREY_UTILS, luaopen_utils},
  {LUA_SREY_HASHRING, luaopen_hashring},
  {LUA_SREY_TREND, luaopen_trend},
  {LUA_SREY_POPEN, luaopen_popen},
  {LUA_SREY_STM, luaopen_stm},
  {LUA_SREY_URL, luaopen_url},
  {LUA_SREY_BASE64, luaopen_base64},
  {LUA_SREY_CRC,  luaopen_crc},
  {LUA_SREY_DIGEST, luaopen_digest},
  {LUA_SREY_HMAC, luaopen_hmac},
  {LUA_SREY_CIPHER, luaopen_cipher},
  {LUA_SREY_SERI, luaopen_seri},
  {LUA_SREY_MYSQL, luaopen_mysql},
  {LUA_SREY_MYSQL_BIND, luaopen_mysql_bind},
  {LUA_SREY_MYSQL_READER, luaopen_mysql_reader},
  {LUA_SREY_MYSQL_STMT, luaopen_mysql_stmt},
  {LUA_SREY_PGSQL, luaopen_pgsql},
  {LUA_SREY_PGSQL_BIND, luaopen_pgsql_bind},
  {LUA_SREY_PGSQL_READER, luaopen_pgsql_reader},
  {LUA_SREY_MQTT, luaopen_mqtt},
  {LUA_SREY_BSON, luaopen_bson},
  {LUA_SREY_BSON_ITER, luaopen_bson_iter},
  {LUA_SREY_MONGO, luaopen_mongo},
  {LUA_SREY_MONGO_SESSION, luaopen_mongo_session},
  {LUA_SREY_KCP, luaopen_kcp},
  {LUA_SREY_ROUTER, luaopen_router},
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
  luaL_requiref(L, LUA_YYJSONLIBNAME, luaopen_yyjson, 1);  /* 脚本不 require 直接用全局 yyjson，仍立即打开 */
  lua_pop(L, 1);
  for (lib = extlibs; lib->name != NULL; lib++) {
    lua_pushcfunction(L, lib->func);
    lua_setfield(L, -2, lib->name);
  }
  lua_pop(L, 1);  /* remove PRELOAD table */
}

