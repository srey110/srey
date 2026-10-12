#ifndef LUAUSER_H_
#define LUAUSER_H_

// 经 lua.h 里的 LUA_USER_H 钩子引入，只有 lualib.vcxproj 定义它，Unix 构建不会进来。
// 给 ldo.c 换 Windows 上 Lua 保护点的跳转：MSVC 的 longjmp 在 x64 / ARM64 上按 SEH 逐帧展开，
// 而 Lua 每次 yield、每次报错都要跳一次。Unix 早已是只恢复寄存器的 _setjmp / _longjmp（mk.sh 的 LUA_USE_POSIX）。
// 前提：会抛 Lua 错误的调用路径上没有 __try / __finally 与 C++ 析构（Lua 与 lbind 全是 C）；
// 工程开 /CETCOMPAT（影子栈）前要先复测

// 是否启用：置 0 退回 Lua 默认的 setjmp / longjmp。放这里而不进 config.h：本头被 Lua 核心每个 .c 引入，
// 引 config.h 会把 os.h 与 Windows 系统头一并带进去（os.h 的空宏 UNUSED 会顶掉 Lua 自己的 UNUSED(x)）
#ifndef ENABLE_LUA_FAST_JMP
    #define ENABLE_LUA_FAST_JMP 1
#endif

#if ENABLE_LUA_FAST_JMP && defined(_MSC_VER) && !defined(__cplusplus)
#include <setjmp.h>
#if defined(_M_X64)
// x64：setjmp 之后把 jmp_buf 的 Frame 清 0，CRT 的 longjmp 就只恢复寄存器、不展开
#define LUAI_THROW(L,c) longjmp((c)->b, 1)
#define LUAI_TRY(L,c,f,ud) if (setjmp((c)->b) == 0) { ((_JUMP_BUFFER *)(c)->b)->Frame = 0; ((f)(L, ud)); }
#elif defined(_M_ARM64)
// ARM64：清 Frame 会崩，改用 luajmp_arm64.asm，只存取被调用方保存的寄存器。
// MSVC 不知道 luajmp_save 会返回两次：唯一调用点 luaD_rawrunprotected 返回后只读 volatile 的 status 与调用前定好的量
int luajmp_save(jmp_buf b);
__declspec(noreturn) void luajmp_restore(jmp_buf b, int v);
#define LUAI_THROW(L,c) luajmp_restore((c)->b, 1)
#define LUAI_TRY(L,c,f,ud) if (luajmp_save((c)->b) == 0) ((f)(L, ud))
#endif
#endif

#endif//LUAUSER_H_
