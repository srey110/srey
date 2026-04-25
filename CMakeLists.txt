cmake_minimum_required(VERSION 3.15)
project(srey C)

# ── Options ───────────────────────────────────────────────────────────────────
option(WITH_SSL "Enable SSL/TLS support" OFF)
option(WITH_LUA "Enable Lua scripting"   ON)

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
        add_compile_options(-flto)
        add_link_options(-flto)
    endif()
endif()

# ── Include directories ───────────────────────────────────────────────────────
set(SREY_INCLUDES
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/lib/base
    ${CMAKE_SOURCE_DIR}/lib/containers
    ${CMAKE_SOURCE_DIR}/lib/crypt
    ${CMAKE_SOURCE_DIR}/lib/event
    ${CMAKE_SOURCE_DIR}/lib/protocol
    ${CMAKE_SOURCE_DIR}/lib/protocol/mongo
    ${CMAKE_SOURCE_DIR}/lib/protocol/mqtt
    ${CMAKE_SOURCE_DIR}/lib/protocol/mysql
    ${CMAKE_SOURCE_DIR}/lib/protocol/pgsql
    ${CMAKE_SOURCE_DIR}/lib/protocol/smtp
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
file(GLOB SRC_CRYPT       lib/crypt/*.c)
file(GLOB SRC_EVENT       lib/event/*.c)
file(GLOB SRC_PROTOCOL    lib/protocol/*.c)
file(GLOB SRC_MONGO       lib/protocol/mongo/*.c)
file(GLOB SRC_MQTT        lib/protocol/mqtt/*.c)
file(GLOB SRC_MYSQL       lib/protocol/mysql/*.c)
file(GLOB SRC_PGSQL       lib/protocol/pgsql/*.c)
file(GLOB SRC_SMTP        lib/protocol/smtp/*.c)
file(GLOB SRC_SERVICES    lib/services/*.c)
file(GLOB SRC_SREY_LIB    lib/srey/*.c)
file(GLOB SRC_THREAD      lib/thread/*.c)
file(GLOB SRC_UTILS       lib/utils/*.c)

set(LIB_SOURCES
    ${SRC_BASE}
    ${SRC_CONTAINERS}
    ${SRC_CRYPT}
    ${SRC_EVENT}
    ${SRC_PROTOCOL}
    ${SRC_MONGO}
    ${SRC_MQTT}
    ${SRC_MYSQL}
    ${SRC_PGSQL}
    ${SRC_SMTP}
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
