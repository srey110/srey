cmake_minimum_required(VERSION 3.15)
project(srey C)

# ── Options ───────────────────────────────────────────────────────────────────
# 注意：WITH_SSL / WITH_LUA 的真正开关在 lib/base/config.h，编译期常量。
# CMake 这里的 option 默认值仅用于影响链接外部库（OpenSSL / Lua）的步骤，
# 切换功能须同步修改 config.h，否则与代码中 #if WITH_SSL 不一致。
option(WITH_SSL "Link OpenSSL libs (config.h 中 WITH_SSL 必须同步)" ON)
option(WITH_LUA "Link Lua libs (config.h 中 WITH_LUA 必须同步)"     ON)

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
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_compile_options(-Wno-unused-function)
elseif(CMAKE_SYSTEM_NAME STREQUAL "AIX")
    add_compile_options(-Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
endif()

# ── Debug / Release ───────────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
else()
    add_compile_options(-O2 -D_FORTIFY_SOURCE=2)
    if(NOT CMAKE_SYSTEM_NAME MATCHES "SunOS|AIX")
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            include(ProcessorCount)
            ProcessorCount(NPROC)
            if(NPROC EQUAL 0)
                set(NPROC 4)
            endif()
            add_compile_options(-flto=${NPROC})
            add_link_options(-flto=${NPROC})
        else()
            add_compile_options(-flto)
            add_link_options(-flto)
        endif()
    endif()
endif()

# ── Include directories ───────────────────────────────────────────────────────
set(SREY_INCLUDES
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/lib/base
    ${CMAKE_SOURCE_DIR}/lib/containers
    ${CMAKE_SOURCE_DIR}/lib/path
    ${CMAKE_SOURCE_DIR}/lib/crypt
    ${CMAKE_SOURCE_DIR}/lib/event
    ${CMAKE_SOURCE_DIR}/lib/protocol
    ${CMAKE_SOURCE_DIR}/lib/protocol/mongo
    ${CMAKE_SOURCE_DIR}/lib/protocol/mqtt
    ${CMAKE_SOURCE_DIR}/lib/protocol/mysql
    ${CMAKE_SOURCE_DIR}/lib/protocol/pgsql
    ${CMAKE_SOURCE_DIR}/lib/protocol/smtp
    ${CMAKE_SOURCE_DIR}/lib/serial
    ${CMAKE_SOURCE_DIR}/lib/services
    ${CMAKE_SOURCE_DIR}/lib/srey
    ${CMAKE_SOURCE_DIR}/lib/thread
    ${CMAKE_SOURCE_DIR}/lib/utils
)
if(WITH_LUA)
    list(APPEND SREY_INCLUDES
        ${CMAKE_SOURCE_DIR}/lualib
        ${CMAKE_SOURCE_DIR}/lualib/lbind
        ${CMAKE_SOURCE_DIR}/lualib/lfs
        ${CMAKE_SOURCE_DIR}/lualib/lua
        ${CMAKE_SOURCE_DIR}/lualib/luacjson
        ${CMAKE_SOURCE_DIR}/lualib/pb
    )
endif()

# ── Core library sources ──────────────────────────────────────────────────────
file(GLOB SRC_BASE        lib/base/*.c)
file(GLOB SRC_CONTAINERS  lib/containers/*.c)
file(GLOB SRC_PATH        lib/path/*.c)
file(GLOB SRC_CRYPT       lib/crypt/*.c)
file(GLOB SRC_EVENT       lib/event/*.c)
file(GLOB SRC_PROTOCOL    lib/protocol/*.c)
file(GLOB SRC_MONGO       lib/protocol/mongo/*.c)
file(GLOB SRC_MQTT        lib/protocol/mqtt/*.c)
file(GLOB SRC_MYSQL       lib/protocol/mysql/*.c)
file(GLOB SRC_PGSQL       lib/protocol/pgsql/*.c)
file(GLOB SRC_SMTP        lib/protocol/smtp/*.c)
file(GLOB SRC_SERIAL      lib/serial/*.c)
file(GLOB SRC_SERVICES    lib/services/*.c)
file(GLOB SRC_SREY_LIB    lib/srey/*.c)
file(GLOB SRC_THREAD      lib/thread/*.c)
file(GLOB SRC_UTILS       lib/utils/*.c)

set(LIB_SOURCES
    ${SRC_BASE}
    ${SRC_CONTAINERS}
    ${SRC_PATH}
    ${SRC_CRYPT}
    ${SRC_EVENT}
    ${SRC_PROTOCOL}
    ${SRC_MONGO}
    ${SRC_MQTT}
    ${SRC_MYSQL}
    ${SRC_PGSQL}
    ${SRC_SMTP}
    ${SRC_SERIAL}
    ${SRC_SERVICES}
    ${SRC_SREY_LIB}
    ${SRC_THREAD}
    ${SRC_UTILS}
)

if(WITH_LUA)
    file(GLOB SRC_LUA       lualib/lua/*.c)
    file(GLOB SRC_LBIND     lualib/lbind/*.c)
    file(GLOB SRC_LFS       lualib/lfs/*.c)
    file(GLOB SRC_LUACJSON  lualib/luacjson/*.c)
    file(GLOB SRC_PB        lualib/pb/*.c)
    list(APPEND LIB_SOURCES
        ${SRC_LUA}
        ${SRC_LBIND}
        ${SRC_LFS}
        ${SRC_LUACJSON}
        ${SRC_PB}
    )
endif()

# ── Static library ────────────────────────────────────────────────────────────
add_library(srey STATIC ${LIB_SOURCES})
target_include_directories(srey PUBLIC ${SREY_INCLUDES})

# ── Platform libraries ────────────────────────────────────────────────────────
set(PLATFORM_LIBS pthread m)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND PLATFORM_LIBS rt dl)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND PLATFORM_LIBS rt)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    list(APPEND PLATFORM_LIBS socket nsl)
endif()
target_link_libraries(srey PUBLIC ${PLATFORM_LIBS})

# ── SSL ───────────────────────────────────────────────────────────────────────
if(WITH_SSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(srey PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# ── Executable: srey ──────────────────────────────────────────────────────────
file(GLOB SRC_SREY_BIN srey/*.c srey/cjson/*.c)
add_executable(srey_bin ${SRC_SREY_BIN})
set_target_properties(srey_bin PROPERTIES OUTPUT_NAME srey)
target_include_directories(srey_bin PRIVATE
    ${SREY_INCLUDES}
    ${CMAKE_SOURCE_DIR}/srey
)
target_link_libraries(srey_bin PRIVATE srey)

# ── Executable: test ──────────────────────────────────────────────────────────
file(GLOB SRC_TEST test/*.c)
add_executable(test_bin ${SRC_TEST})
set_target_properties(test_bin PROPERTIES OUTPUT_NAME test)
target_include_directories(test_bin PRIVATE
    ${SREY_INCLUDES}
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(test_bin PRIVATE srey)
