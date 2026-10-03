#ifndef CONTENTTYPE_H_
#define CONTENTTYPE_H_

#include "base/base.h"

/// <summary>
/// 按文件扩展名查 MIME 类型。内部是一张按扩展名升序排好的静态表 + bsearch,
/// 新增条目必须插到正确位置, 顺序乱掉查找就会漏
/// </summary>
/// <param name="extension">扩展名字.xx</param>
/// <returns>Content-Type;查不到返回 "application/X-other-1"</returns>
const char *contenttype(const char *extension);

#endif//CONTENTTYPE_H_
