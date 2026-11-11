#ifndef CONTENTTYPE_H_
#define CONTENTTYPE_H_

#include "base/base.h"

/// <summary>
/// 按文件扩展名查 MIME 类型，不区分大小写
/// </summary>
/// <param name="extension">扩展名，带前导 '.'（如 ".html"），不可为 NULL</param>
/// <returns>Content-Type;查不到返回 "application/octet-stream"</returns>
const char *contenttype(const char *extension);

#endif//CONTENTTYPE_H_
