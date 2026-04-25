#ifndef PGSQL_PACK_H_
#define PGSQL_PACK_H_

#include "utils/binary.h"
#include "protocol/pgsql/pgsql_struct.h"

// 命令打包辅助函数
/// <summary>
/// 开始构建一个新的 pgsql 消息（初始化写缓冲区并写入消息类型码和长度占位符）
/// </summary>
/// <param name="bwriter">写缓冲区</param>
/// <param name="code">消息类型码（如 'Q'、'P' 等）</param>
void pgsql_pack_start(binary_ctx *bwriter, int8_t code);
/// <summary>
/// 结束消息构建并回填消息体长度字段
/// </summary>
/// <param name="bwriter">写缓冲区</param>
void pgsql_pack_end(binary_ctx *bwriter);
/// <summary>
/// 在已有缓冲区末尾追加一个新子消息的起始（写入类型码和长度占位符）
/// </summary>
/// <param name="bwriter">写缓冲区</param>
/// <param name="code">子消息类型码</param>
/// <returns>长度字段的偏移量，供 pgsql_pack_append_end 回填使用</returns>
size_t pgsql_pack_append_start(binary_ctx *bwriter, int8_t code);
/// <summary>
/// 结束追加子消息并回填其长度字段
/// </summary>
/// <param name="bwriter">写缓冲区</param>
/// <param name="offset">pgsql_pack_append_start 返回的长度字段偏移量</param>
void pgsql_pack_append_end(binary_ctx *bwriter, size_t offset);

// 命令打包
/// <summary>
/// 打包 Terminate 消息（通知服务端关闭连接）
/// </summary>
/// <param name="size">输出消息字节数</param>
/// <returns>消息数据指针，调用方负责释放</returns>
void *pgsql_pack_terminate(size_t *size);
/// <summary>
/// 打包简单查询消息（Query）
/// </summary>
/// <param name="sql">SQL 语句字符串</param>
/// <param name="size">输出消息字节数</param>
/// <returns>消息数据指针，调用方负责释放</returns>
void *pgsql_pack_query(const char *sql, size_t *size);
/// <summary>
/// 打包预处理语句的 Parse + Sync 消息
/// </summary>
/// <param name="name">预处理语句名称，空字符串表示匿名语句</param>
/// <param name="sql">SQL 语句字符串</param>
/// <param name="nparam">参数数量</param>
/// <param name="oids">各参数的类型 OID 数组，可为 NULL</param>
/// <param name="size">输出消息字节数</param>
/// <returns>消息数据指针，调用方负责释放</returns>
void *pgsql_pack_stmt_prepare(const char *name, const char *sql, int16_t nparam, uint32_t *oids, size_t *size);
/// <summary>
/// 打包 Bind + Describe + Execute + Sync 消息，用于执行预处理语句
/// </summary>
/// <param name="name">预处理语句名称</param>
/// <param name="bind">参数绑定上下文，无参数时可为 NULL</param>
/// <param name="resultformat">结果列的格式（文本或二进制）</param>
/// <param name="size">输出消息字节数</param>
/// <returns>消息数据指针，调用方负责释放</returns>
void *pgsql_pack_stmt_execute(const char *name, pgsql_bind_ctx *bind, pgpack_format resultformat, size_t *size);
/// <summary>
/// 打包 Close + Sync 消息，用于关闭预处理语句
/// </summary>
/// <param name="name">预处理语句名称</param>
/// <param name="size">输出消息字节数</param>
/// <returns>消息数据指针，调用方负责释放</returns>
void *pgsql_pack_stmt_close(const char *name, size_t *size);

#endif//PGSQL_PACK_H_
