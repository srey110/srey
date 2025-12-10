/*
** $Id: lualib.h $
** Lua standard libraries
** See Copyright Notice in lua.h
*/


#ifndef lualib_h
#define lualib_h

#include "lua.h"


/* version suffix for environment variable names */
#define LUA_VERSUFFIX          "_" LUA_VERSION_MAJOR "_" LUA_VERSION_MINOR


LUAMOD_API int (luaopen_base) (lua_State *L);

#define LUA_COLIBNAME	"coroutine"
LUAMOD_API int (luaopen_coroutine) (lua_State *L);

#define LUA_TABLIBNAME	"table"
LUAMOD_API int (luaopen_table) (lua_State *L);

#define LUA_IOLIBNAME	"io"
LUAMOD_API int (luaopen_io) (lua_State *L);

#define LUA_OSLIBNAME	"os"
LUAMOD_API int (luaopen_os) (lua_State *L);

#define LUA_STRLIBNAME	"string"
LUAMOD_API int (luaopen_string) (lua_State *L);

#define LUA_UTF8LIBNAME	"utf8"
LUAMOD_API int (luaopen_utf8) (lua_State *L);

#define LUA_MATHLIBNAME	"math"
LUAMOD_API int (luaopen_math) (lua_State *L);

#define LUA_DBLIBNAME	"debug"
LUAMOD_API int (luaopen_debug) (lua_State *L);

#define LUA_LOADLIBNAME	"package"
LUAMOD_API int (luaopen_package) (lua_State *L);

#define LUA_LFS "lfs"
LUAMOD_API int (luaopen_lfs)(lua_State *L);
#define LUA_CJSONLIBNAME "cjson"
LUAMOD_API int (luaopen_cjson)(lua_State *L);
#define LUA_PBPACKNAME "pb"
LUAMOD_API int (luaopen_pb)(lua_State *L);

#define LUA_SREYTASK "srey.task"
LUAMOD_API int (luaopen_task)(lua_State *L);
#define LUA_SREYCORE "srey.core"
LUAMOD_API int (luaopen_core)(lua_State *L);
#define LUA_SREYHARBOR "srey.harbor"
LUAMOD_API int (luaopen_harbor)(lua_State *L);
#define LUA_SREYDNS "srey.dns"
LUAMOD_API int (luaopen_dns)(lua_State *L);
#define LUA_SREYCUSTZ "srey.custz"
LUAMOD_API int (luaopen_custz)(lua_State *L);
#define LUA_SREYWEBSOCK "srey.websock"
LUAMOD_API int (luaopen_websock)(lua_State *L);
#define LUA_SREYHTTP "srey.http"
LUAMOD_API int (luaopen_http)(lua_State *L);
#define LUA_SREYREDIS "srey.redis"
LUAMOD_API int (luaopen_redis)(lua_State *L);
#define LUA_SMTP "srey.smtp"
LUAMOD_API int (luaopen_smtp)(lua_State *L);
#define LUA_SMTP_MAIL "srey.smtp.mail"
LUAMOD_API int (luaopen_mail)(lua_State *L);
#define LUA_SREYUTILS "srey.utils"
LUAMOD_API int (luaopen_utils)(lua_State *L);
#define LUA_SREYHASHRING "srey.hashring"
LUAMOD_API int (luaopen_hashring)(lua_State *L);
#define LUA_SREYURL "srey.url"
LUAMOD_API int (luaopen_url)(lua_State *L);
#define LUA_SREYBASE64 "srey.base64"
LUAMOD_API int (luaopen_base64)(lua_State *L);
#define LUA_SREYCRC "srey.crc"
LUAMOD_API int (luaopen_crc)(lua_State *L);
#define LUA_SREYDIGEST "srey.digest"
LUAMOD_API int (luaopen_digest)(lua_State *L);
#define LUA_SREYHMAC "srey.hmac"
LUAMOD_API int (luaopen_hmac)(lua_State *L);
#define LUA_SREYCIPHER "srey.cipher"
LUAMOD_API int (luaopen_cipher)(lua_State *L);
#define LUA_MYSQLBIND "mysql.bind"
LUAMOD_API int (luaopen_mysql_bind)(lua_State *L);
#define LUA_MYSQLREADER "mysql.reader"
LUAMOD_API int (luaopen_mysql_reader)(lua_State *L);
#define LUA_MYSQLSTMT "mysql.stmt"
LUAMOD_API int (luaopen_mysql_stmt)(lua_State *L);
#define LUA_MYSQL "mysql"
LUAMOD_API int (luaopen_mysql)(lua_State *L);

/* open all previous libraries */
LUALIB_API void (luaL_openlibs) (lua_State *L);


#endif
