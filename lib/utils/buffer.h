#ifndef BUFFER_H_
#define BUFFER_H_

#include "base/structs.h"

//�������ڴ��д
#if defined(OS_WIN)
#define IOV_TYPE WSABUF
#define IOV_PTR_FIELD buf
#define IOV_LEN_FIELD len
#define IOV_LEN_TYPE ULONG
#else
//struct iovec {
//    void *iov_base;
//    size_t iov_len;
//};
#define IOV_TYPE struct iovec
#define IOV_PTR_FIELD iov_base
#define IOV_LEN_FIELD iov_len
#define IOV_LEN_TYPE size_t
#endif
#define MAX_EXPAND_NIOV          4

typedef struct buffer_ctx {
    volatile int32_t freeze_read;
    volatile int32_t freeze_write;
    struct bufnode_ctx *head;
    struct bufnode_ctx *tail;
    struct bufnode_ctx **tail_with_data;
    size_t total_lens;//�����ܳ���
    struct bufnode_ctx *hint_node;    // 搜索游标：上次命中的节点
    size_t             hint_base_off; // 该节点之前所有节点的累计字节数
}buffer_ctx;
/// <summary>
/// �������ڴ��ʼ��
/// </summary>
/// <param name="ctx">buffer_ctx</param>
void buffer_init(buffer_ctx *ctx);
/// <summary>
/// �������ڴ��ͷ�
/// </summary>
/// <param name="ctx">buffer_ctx</param>
void buffer_free(buffer_ctx *ctx);
/// <summary>
/// ��ȡ���ݳ���
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <returns>����</returns>
size_t buffer_size(buffer_ctx *ctx);
/// <summary>
/// ���ⲿ����data���ӵ�buffer,����һ�ο���,��������Ķ�ȡ��
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="data">����</param>
/// <param name="lens">����</param>
/// <param name="free_cb">data�ͷź���</param>
void buffer_external(buffer_ctx *ctx, void *data, const size_t lens, free_cb _free);
/// <summary>
/// д������
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="data">����</param>
/// <param name="lens">����</param>
/// <returns>ERR_OK �ɹ�</returns>
int32_t buffer_append(buffer_ctx *ctx, void *data, const size_t lens);
/// <summary>
/// д������
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="fmt">��ʽ��</param>
/// <param name="...">���</param>
/// <returns>ERR_OK �ɹ�</returns>
int32_t buffer_appendv(buffer_ctx *ctx, const char *fmt, ...);
/// <summary>
/// ��ȡ����
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="start">��ʼλ��</param>
/// <param name="out">����</param>
/// <param name="lens">��ȡ����</param>
/// <returns>ʵ�ʶ�ȡ���ĳ���</returns>
size_t buffer_copyout(buffer_ctx *ctx, const size_t start, void *out, size_t lens);
/// <summary>
/// ɾ������
/// </summary>
/// <param name="lens">����</param>
/// <returns>ʵ��ɾ���ĳ���</returns>
size_t buffer_drain(buffer_ctx *ctx, size_t lens);
/// <summary>
/// ��ȡ����ɾ������
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="out">����</param>
/// <param name="lens">����</param>
/// <returns>ʵ�ʳ���</returns>
size_t buffer_remove(buffer_ctx *ctx, void *out, size_t lens);
/// <summary>
/// ����
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="ncs">0 ���ִ�Сд</param>
/// <param name="start">��ʼ����λ��</param>
/// <param name="end">��������λ��, 0 ֱ�����ݽ���</param>
/// <param name="what">Ҫ����������</param>
/// <param name="wlens">���������ݳ���</param>
/// <returns>ERR_FAILED ʧ�� �����������ڿ�ʼλ��</returns>
int32_t buffer_search(buffer_ctx *ctx, const int32_t ncs,
    const size_t start, size_t end, char *what, size_t wlens);
/// <summary>
/// ��ȡָ��λ��ֵ
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="pos">λ��</param>
/// <returns>char</returns>
char buffer_at(buffer_ctx *ctx, size_t pos);
/// <summary>
/// ��ȡָ���������ڴ�,����д��
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="lens">Ҫ��ȡ�Ĵ�С</param>
/// <param name="iov">IOV����</param>
/// <param name="cnt">IOV���鳤��</param>
/// <returns>IOV����</returns>
uint32_t buffer_expand(buffer_ctx *ctx, const size_t lens, IOV_TYPE *iov, const uint32_t cnt);
/// <summary>
/// buffer_expand �ύд��
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="lens">���ݳ���</param>
/// <param name="iov">IOV����</param>
/// <param name="cnt">IOV���鳤��</param>
void buffer_commit_expand(buffer_ctx *ctx, size_t lens ,IOV_TYPE *iov, const uint32_t cnt);
/// <summary>
/// ��ȡָ�����ȵ�����
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="atmost">����</param>
/// <param name="iov">IOV����</param>
/// <param name="cnt">IOV���鳤��</param>
/// <returns>IOV����</returns>
uint32_t buffer_get(buffer_ctx *ctx, size_t atmost, IOV_TYPE *iov, const uint32_t cnt);
/// <summary>
/// buffer_get ����ɾ��
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="size">����</param>
void buffer_commit_get(buffer_ctx *ctx, size_t size);
/// <summary>
/// ��socket�ж�������
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="fd">socket���</param>
/// <param name="nread">��ȡ���ĳ���</param>
/// <param name="_readv">��ȡ����</param>
/// <param name="arg">����</param>
/// <returns>ERR_OK �ɹ�</returns>
int32_t buffer_from_sock(buffer_ctx *ctx, SOCKET fd, size_t *nread,
    int32_t(*_readv)(SOCKET, IOV_TYPE *, uint32_t, void *, size_t *), void *arg);

#endif//BUFFER_H_
