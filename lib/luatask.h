#ifndef LUATASK_H_
#define LUATASK_H_

#include "srey.h"
#include "lua.h"

void lua_initpath();
lua_State *lua_newfile(const char *pfile);

#endif//LUATASK_H_
