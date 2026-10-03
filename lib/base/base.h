#ifndef BASE_H_
#define BASE_H_

// 汇总头：base 之外一律 include 它，拿到整套 base。自己不定义任何东西；base 里除 structs.h 外的头不许引它，按需点名引下层
// 分层(每个头只引比自己低的层)：os.h(末尾引 os_unix.h / os_win.h)、err.h、config.h → macro_util.h、macro_atomic.h
// → memory.h、sbyteswap.h、bits.h、strconv.h → bytes.h → macro_log.h → 本文件 → structs.h
#include "base/os.h"
#include "base/err.h"
#include "base/config.h"
#include "base/macro_util.h"
#include "base/macro_atomic.h"
#include "base/memory.h"
#include "base/sbyteswap.h"
#include "base/bits.h"
#include "base/bytes.h"
#include "base/strconv.h"
#include "base/macro_log.h"

#endif//BASE_H_
