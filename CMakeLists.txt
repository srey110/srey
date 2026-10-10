cmake_minimum_required(VERSION 3.15)
project(srey C)

# 与 mk.sh 同口径的 Unix 构建；Windows 用 mk.bat / srey.sln。
# 第三方依赖先跑 python3 tools/deps.py(Debug 配 deps.py debug)，头在 lib/、静态库在 bin/

# ── Options ───────────────────────────────────────────────────────────────────
# 功能开关(WITH_SSL / WITH_LUA / WITH_MIMALLOC)只认 lib/base/config.h，这里读出来决定链什么，不另设 option
file(STRINGS ${CMAKE_SOURCE_DIR}/lib/base/config.h _cfg_lines REGEX "^[ \t]*#define[ \t]+WITH_(SSL|LUA|MIMALLOC)[ \t]+[0-9]+")
foreach(_line ${_cfg_lines})
    string(REGEX MATCH "WITH_(SSL|LUA|MIMALLOC)[ \t]+([0-9]+)" _m "${_line}")
    set(SREY_WITH_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
endforeach()
option(SREY_ASAN "ASan/UBSan(macOS ARM64 须配 Debug)" OFF)
option(SREY_TSAN "ThreadSanitizer，与 SREY_ASAN 互斥" OFF)
option(SREY_RDYNAMIC "Linux 链接加 -rdynamic，stack_print 的栈带函数名(理由见 mk.sh)" OFF)
if(SREY_ASAN AND SREY_TSAN)
    message(FATAL_ERROR "SREY_ASAN 与 SREY_TSAN 不能同时启用")
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# 只认 Debug / Release(不分大小写)，对应 mk.sh 的两套参数；RelWithDebInfo 之类会带上 CMake 自己的 -O / -g / -DNDEBUG
string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type)
if(_build_type STREQUAL "DEBUG")
    set(CMAKE_BUILD_TYPE Debug)
elseif(_build_type STREQUAL "RELEASE")
    set(CMAKE_BUILD_TYPE Release)
else()
    message(FATAL_ERROR "CMAKE_BUILD_TYPE 只支持 Debug / Release，收到的是 ${CMAKE_BUILD_TYPE}")
endif()

# ── C standard (gnu99) ────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)

# ── Output directories ────────────────────────────────────────────────────────
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# ── Compiler warnings ─────────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wshadow
    -Wstrict-prototypes -Wold-style-definition
    -Wpointer-arith
    -Werror=implicit-function-declaration
)
# 暴露 glibc GNU 扩展（epoll_create1 / F_GETPIPE_SZ 等），对非 glibc 平台无副作用
add_compile_definitions(_GNU_SOURCE)
if(SREY_WITH_LUA)
    add_compile_definitions(LUA_USE_POSIX)# 理由见 mk.sh
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Darwin|FreeBSD")
    add_compile_options(-Wno-unused-function)
elseif(CMAKE_SYSTEM_NAME STREQUAL "AIX")
    add_compile_options(-Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(__const__=__unused__)# 协程换线程后 errno 地址不能复用，理由见 mk.sh，别删
endif()

# ── Debug / Release ───────────────────────────────────────────────────────────
# 清掉 CMake 自带的 -O3 / -DNDEBUG / -g，只用下面这套，与 mk.sh 一致
set(CMAKE_C_FLAGS_DEBUG "")
set(CMAKE_C_FLAGS_RELEASE "")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g3)
else()
    add_compile_options(-O2)
    add_compile_definitions(_FORTIFY_SOURCE=2)
    if(NOT CMAKE_SYSTEM_NAME MATCHES "SunOS|AIX")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok)
        # LTO 交给 CMake(gcc 要的 gcc-ar / gcc-ranlib 由它自己换)。它选的 LTO 方式未必与 mk.sh 的 -flto 相同，
        # 性能对比以 mk.sh 的产物为准
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${_ipo_ok})
    endif()
endif()
if(SREY_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
if(SREY_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_compile_definitions(BUILD_TSAN=1)
    add_link_options(-fsanitize=thread)
endif()

# ── 线程局部变量门禁(同 mk.sh) ──────────────────────────────────────────────
# 只许经 TLS_DEFINE 定义(理由见 lib/base/macro_util.h)，确需裸写的行尾注明 TLS_RAW_OK。每次构建前查一遍
add_custom_target(tls_gate
    COMMAND sh -c "! find lib srey lualib/lbind test \\( -name '*.c' -o -name '*.h' \\) -exec grep -nE '(^|[^[:alnum:]_])(THREAD_LOCAL|_Thread_local|__thread|thread_local)([^[:alnum:]_]|$)|__declspec *\\( *thread *\\)' /dev/null {} + | grep -vE '^lib/(base/(macro_util|os_unix|os_win)\\.h|coro/minicoro\\.h|openssl/|mimalloc/|serial/yyjson/)' | grep -v TLS_RAW_OK"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "检查裸写的线程局部变量：有输出即违规，改用 TLS_DEFINE"
    VERBATIM
)

# ── Include directories / sources ─────────────────────────────────────────────
set(SREY_DIRS
    lib lib/base lib/utils lib/containers lib/crypt lib/event lib/serial lib/serial/yyjson lib/coro lib/srey lib/thread
    lib/protocol lib/protocol/mongo lib/protocol/mqtt lib/protocol/mysql lib/protocol/pgsql lib/protocol/smtp lib/protocol/kcp
    lib/advance
)
if(SREY_WITH_LUA)
    list(APPEND SREY_DIRS lualib lualib/lua lualib/lbind)
endif()
set(SREY_INCLUDES "")
set(LIB_SOURCES "")
foreach(_dir ${SREY_DIRS})
    list(APPEND SREY_INCLUDES ${CMAKE_SOURCE_DIR}/${_dir})
    file(GLOB _src CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/${_dir}/*.c)
    list(APPEND LIB_SOURCES ${_src})
endforeach()

# ── Static library ────────────────────────────────────────────────────────────
add_library(srey STATIC ${LIB_SOURCES})
target_include_directories(srey PUBLIC ${SREY_INCLUDES})
add_dependencies(srey tls_gate)

# ── Third-party libs (tools/deps.py) ──────────────────────────────────────────
# 库名带变体后缀「[d]_<x86|x64|arm64>」，规则同 mk.sh；对不上就报错，不静默链别的变体
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(_deps_arch x86)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(_deps_arch arm64)
else()
    set(_deps_arch x64)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_deps_sfx d_${_deps_arch})
else()
    set(_deps_sfx _${_deps_arch})
endif()
set(_deps_names "")
if(SREY_WITH_SSL)
    list(APPEND _deps_names ssl crypto)
endif()
if(SREY_WITH_MIMALLOC)
    list(APPEND _deps_names mimalloc)# 排在 srey 之后：引用方是 libsrey.a 里的 memory.o
endif()
foreach(_name ${_deps_names})
    set(_lib ${CMAKE_SOURCE_DIR}/bin/lib${_name}${_deps_sfx}.a)
    if(NOT EXISTS ${_lib})
        message(FATAL_ERROR "缺 ${_lib}，先跑 python3 tools/deps.py(Debug 加 debug)")
    endif()
    target_link_libraries(srey PUBLIC ${_lib})
endforeach()

# ── Platform libraries ────────────────────────────────────────────────────────
set(PLATFORM_LIBS pthread m)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND PLATFORM_LIBS rt)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND PLATFORM_LIBS dl)
    if(SREY_RDYNAMIC)
        list(APPEND PLATFORM_LIBS -rdynamic)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    list(APPEND PLATFORM_LIBS socket nsl)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    list(APPEND PLATFORM_LIBS execinfo)# backtrace 在独立的 libexecinfo
endif()
target_link_libraries(srey PUBLIC ${PLATFORM_LIBS})

# ── Executable: srey ──────────────────────────────────────────────────────────
file(GLOB SRC_SREY_BIN CONFIGURE_DEPENDS srey/*.c)
add_executable(srey_bin ${SRC_SREY_BIN})
set_target_properties(srey_bin PROPERTIES OUTPUT_NAME srey)
target_include_directories(srey_bin PRIVATE ${CMAKE_SOURCE_DIR}/srey)
target_link_libraries(srey_bin PRIVATE srey)

# ── Executable: test ──────────────────────────────────────────────────────────
file(GLOB SRC_TEST CONFIGURE_DEPENDS test/*.c)
add_executable(test_bin ${SRC_TEST})
set_target_properties(test_bin PROPERTIES OUTPUT_NAME test)
target_include_directories(test_bin PRIVATE ${CMAKE_SOURCE_DIR}/test)
target_link_libraries(test_bin PRIVATE srey)
