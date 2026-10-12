#ifndef SFILE_H_
#define SFILE_H_

#include "base/os.h"

/// <summary>
/// 是否为文件
/// </summary>
/// <param name="file">路径</param>
/// <returns>ERR_OK 文件</returns>
int32_t isfile(const char *file);
/// <summary>
/// 是否为文件夹
/// </summary>
/// <param name="path">路径</param>
/// <returns>ERR_OK 文件夹</returns>
int32_t isdir(const char *path);
/// <summary>
/// 文件大小
/// </summary>
/// <param name="file">路径</param>
/// <returns>文件大小, ERR_FAILED 失败</returns>
int64_t filesize(const char *file);
/// <summary>
/// 获取文件最后修改时间
/// </summary>
/// <param name="file">路径</param>
/// <returns>修改时间(秒);失败返回 0</returns>
uint64_t file_mtime(const char *file);
/// <summary>
/// 打开文件并标记为子进程不可继承。参数与 fopen 相同。本项目自己的 fopen 都该走它:
/// popen 起的子进程会连带继承进程里每一个可继承的句柄。
/// 覆盖有两处已知缺口:第三方代码自带的 fopen 不走这里;打标与 fopen 不是一步,
/// 两者之间那一小段窗口里 fork 出去的子进程仍会继承这个句柄
/// </summary>
/// <param name="file">路径</param>
/// <param name="mode">fopen 的模式串</param>
/// <returns>文件流，需调用方 fclose；失败返回 NULL 并置 errno</returns>
FILE *fopen_cloexec(const char *file, const char *mode);
/// <summary>
/// 读取文件全部。所有失败路径都会置 errno，调用方可直接 strerror——空文件与读取途中被截断
/// 这两种情形本身不来自系统调用，函数会自己补上 errno，不会留下上一次系统调用的陈旧值
/// </summary>
/// <param name="file">路径</param>
/// <param name="lens">文件大小；仅成功时被写入</param>
/// <returns>文件内容，需调用方 FREE；失败返回 NULL 并置 errno</returns>
char *readall(const char *file, size_t *lens);

#endif//SFILE_H_
