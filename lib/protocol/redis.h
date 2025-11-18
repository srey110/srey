#ifndef REDIS_H_
#define REDIS_H_

#include "utils/buffer.h"

#define RESP_STRING  '+' //+  Simple strings   RESP2  +OK\r\n
#define RESP_ERROR   '-' //-  Simple Errors    RESP2  -Error message\r\n
#define RESP_INTEGER ':' //:  Integers         RESP2  :[<+|->]<value>\r\n
#define RESP_NIL     '_' //_  Nulls            RESP3  _\r\n
#define RESP_BOOL    '#' //#  Booleans         RESP3  #<t|f>\r\n
#define RESP_DOUBLE  ',' //,  Doubles          RESP3  ,[<+|->]<integral>[.<fractional>][<E|e>[sign]<exponent>]\r\n
#define RESP_BIGNUM  '(' //(  Big numbers      RESP3  ([+|-]<number>\r\n

#define RESP_BSTRING '$' //$  Bulk strings     RESP2  $<length>\r\n<data>\r\n
#define RESP_BERROR  '!' //!  Bulk errors      RESP3  !<length>\r\n<error>\r\n
#define RESP_VERB    '=' //=  Verbatim strings RESP3  =<length>\r\n<encoding>:<data>\r\n

#define RESP_ARRAY   '*' //*  Arrays           RESP2  *<number-of-elements>\r\n<element-1>...<element-n>
#define RESP_SET     '~' //~  Sets             RESP3  ~<number-of-elements>\r\n<element-1>...<element-n>
#define RESP_PUSHE   '>' //>  Pushes           RESP3  ><number-of-elements>\r\n<element-1>...<element-n>
#define RESP_MAP     '%' //%  Maps             RESP3  %<number-of-entries>\r\n<key-1><value-1>...<key-n><value-n>
#define RESP_ATTR    '|' //|  Attribute type   RESP3  like the Map type, but instead of the % first byte, the | byte is used.
                                                    //however the client should not consider such a dictionary part of the reply, 
                                                    //but just auxiliary data that is used in order to augment the reply.
typedef struct redis_pack_ctx {
    int32_t proto;
    char venc[4]; //RESP_VERB  encoding
    int64_t nelem;//多少条数据 RESP_ARRAY RESP_SET RESP_PUSHE RESP_MAP  RESP_ATTR
    int64_t len;//data 长度
    int64_t ival;//RESP_INTEGER RESP_BOOL RESP_BIGNUM 
    double dval;//RESP_DOUBLE
    struct redis_pack_ctx *next;
    char data[0];//RESP_STRING RESP_ERROR RESP_BSTRING RESP_BERROR RESP_VERB
}redis_pack_ctx;

void _redis_pkfree(redis_pack_ctx *pack);
void _redis_udfree(ud_cxt *ud);
/// <summary>
/// 请求包
/// </summary>
/// <param name="size">请求包长度</param>
/// <param name="fmt">格式化 参数以空格分割, %b:binary - size_t %%:%  C format</param>
/// <param name="...">变参</param>
/// <returns>请求包</returns>
char *redis_pack(size_t *size, const char *fmt, ...);
//解包
redis_pack_ctx *redis_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *status);

#endif//REDIS_H_
