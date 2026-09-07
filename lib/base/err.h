#ifndef ERR_H_
#define ERR_H_

#define ERR_OK       0 // 操作成功
#define ERR_FAILED  -1 // 操作失败

// CLOSE 消息 erro 的取值：这条 CLOSE 是怎么来的，五档互斥。
// 取正数是为了与 ERR_FAILED 及后续可能新增的负错误码分开；上层按数值镜像这张表，
// 新档一律追加在末尾，插在中间会让镜像错位
typedef enum close_type {
    CLOSE_TYPE_ORDERLY = ERR_OK, // 对端有序结束发送方向：裸 TCP 收到 FIN，SSL 收到 close_notify
    CLOSE_TYPE_LOCAL,            // 本地主动或本地故障：ev_close / task 拆除 / 发队列溢出 / 解析错误 / 事件注册失败
    CLOSE_TYPE_ABORT,            // 异常中断：RST、读写错误、SSL 协议错
    CLOSE_TYPE_NEVERCONN,        // 连接/会话从未建立，本消息只为唤醒等待方，分发层据此跳过 on_close
    CLOSE_TYPE_TRUNCATED         // TLS 没发 close_notify 就断了：字节可能已收全，也可能被中间人截断，本层分不出
}close_type;

#define ERRSTR_NULLP      "null pointer." // 空指针错误描述
#define ERRSTR_INVPARAM   "invalid parameter." // 无效参数错误描述

#endif//ERR_H_
