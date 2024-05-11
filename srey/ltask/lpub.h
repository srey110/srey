#ifndef LPUB_H_
#define LPUB_H_

#include "lib.h"
#if WITH_LUA
#include "lua/lualib.h"
#include "lua/lauxlib.h"

#define SCRIPT_FOLDER "script"
#define CERT_FOLDER "keys"
#define CUR_TASK_NAME "_curtask"
#define PATH_NAME "_propath"
#define PATH_SEP_NAME "_pathsep"
#define MSG_DISP_FUNC "message_dispatch"

void *global_userdata(lua_State *lua, const char *name);
const char *global_string(lua_State *lua, const char *name);

#endif
#endif//LPUB_H_
