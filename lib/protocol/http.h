#ifndef HTTP_H_
#define HTTP_H_

#include "utils/buffer.h"
#include "utils/binary.h"
#include "utils/utils.h"

typedef struct http_header_ctx {
    buf_ctx key;
    buf_ctx value;
}http_header_ctx;
struct http_pack_ctx;

void _http_pkfree(struct http_pack_ctx *pack);
void _http_udfree(ud_cxt *ud);
//http���
struct http_pack_ctx *http_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// ��ȡ״̬���Ӧ������
/// </summary>
/// <param name="code">״̬��</param>
/// <returns>����</returns>
const char *http_code_status(int32_t code);
/// <summary>
/// http�����
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="method">���� GET POST...</param>
/// <param name="url">url</param>
void http_pack_req(binary_ctx *bwriter, const char *method, const char *url);
/// <summary>
/// http���ذ�
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="code">״̬��</param>
void http_pack_resp(binary_ctx *bwriter, int32_t code);
/// <summary>
/// httpͷ
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="key">����</param>
/// <param name="val">ֵ</param>
void http_pack_head(binary_ctx *bwriter, const char *key, const char *val);
/// <summary>
/// http������, ֻ��ͷ��ʱ��
/// </summary>
/// <param name="bwriter">binary_ctx</param>
void http_pack_end(binary_ctx *bwriter);
/// <summary>
/// http������
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="data">����</param>
/// <param name="lens">����</param>
void http_pack_content(binary_ctx *bwriter, void *data, size_t lens);
/// <summary>
/// http������,Chunked ��ʽ. ѭ�� send(...) copy 1; binary_offset(bwriter, 0);
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="data">����</param>
/// <param name="lens">����</param>
void http_pack_chunked(binary_ctx *bwriter, void *data, size_t lens);

struct http_pack_ctx *_http_parsehead(buffer_ctx *buf, int32_t *transfer, int32_t *status);
/* Check that a parsed header's key matches <key> (case-insensitive, length klen)
 * and, when val != NULL, that the value contains <val> (length vlen).
 * Accepting explicit lengths lets callers use sizeof(literal)-1 (compile-time
 * constant) and enables inlining since the function is tiny. */
static inline int32_t _http_check_keyval(http_header_ctx *head,
                                          const char *key, size_t klen,
                                          const char *val, size_t vlen) {
    if (!buf_icompare(&head->key, key, klen)) {
        return ERR_FAILED;
    }
    if (NULL == val) {
        return ERR_OK;
    }
    if (NULL != memstr(1, head->value.data, head->value.lens, val, vlen)) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
/// <summary>
/// ��ȡ��һ������
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>buf_ctx</returns>
buf_ctx *http_status(struct http_pack_ctx *pack);
/// <summary>
/// ��ȡͷ����
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>����</returns>
uint32_t http_nheader(struct http_pack_ctx *pack);
/// <summary>
/// ��ȡͷ
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="pos">�ڼ���</param>
/// <returns>http_header_ctx</returns>
http_header_ctx *http_header_at(struct http_pack_ctx *pack, uint32_t pos);
/// <summary>
/// ��ȡͷ
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="header">����</param>
/// <param name="lens">ֵ����</param>
/// <returns>ֵ</returns>
char *http_header(struct http_pack_ctx *pack, const char *header, size_t *lens);
/// <summary>
/// chunked
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>0 ��chunked, 1 chunked��ʼ 2 chunked���ݰ�</returns>
int32_t http_chunked(struct http_pack_ctx *pack);
/// <summary>
/// ��ȡ���ݰ�
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="lens">���ݰ�����</param>
/// <returns>���ݰ�</returns>
void *http_data(struct http_pack_ctx *pack, size_t *lens);

#endif//HTTP_H_
