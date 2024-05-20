#ifndef REDIS_H_
#define REDIS_H_

#include "structs.h"
#include "buffer.h"

typedef enum resp_type {
    RESP_NONE = 0x00,
    RESP_STRING,       //+  Simple strings   RESP2  +OK\r\n
    RESP_ERROR,        //-  Simple Errors    RESP2  -Error message\r\n
    RESP_INTEGER,      //:  Integers         RESP2  :[<+|->]<value>\r\n
    RESP_NULL,         //_  Nulls            RESP3  _\r\n
    RESP_BOOL,         //#  Booleans         RESP3  #<t|f>\r\n
    RESP_DOUBLE,       //,  Doubles          RESP3  ,[<+|->]<integral>[.<fractional>][<E|e>[sign]<exponent>]\r\n
    RESP_BIG_NUMBER,   //(  Big numbers      RESP3  ([+|-]<number>\r\n

    RESP_BULK_STRING,  //$  Bulk strings     RESP2  $<length>\r\n<data>\r\n
    RESP_BULK_ERROR,   //!  Bulk errors      RESP3  !<length>\r\n<error>\r\n
    RESP_VERB_STRING,  //=  Verbatim strings RESP3  =<length>\r\n<encoding>:<data>\r\n

    RESP_ARRAY,        //*  Arrays           RESP2  *<number-of-elements>\r\n<element-1>...<element-n>
    RESP_MAP,          //%  Maps             RESP3  %<number-of-entries>\r\n<key-1><value-1>...<key-n><value-n>
    RESP_SET,          //~  Sets             RESP3  ~<number-of-elements>\r\n<element-1>...<element-n>
    RESP_PUSHE,        //>  Pushes           RESP3  ><number-of-elements>\r\n<element-1>...<element-n>
}resp_type;
typedef struct redis_pack_ctx {
    resp_type type;
    char vtype[4]; //RESP_VERB_STRING  encoding
    size_t element;//多少条数据 RESP_ARRAY RESP_MAP  RESP_SET RESP_PUSHE
    size_t len;//data 长度
    int64_t ival;//RESP_INTEGER RESP_BOOL RESP_BIG_NUMBER 
    double dval;//RESP_DOUBLE
    struct redis_pack_ctx *next;
    char data[0];//RESP_STRING RESP_ERROR RESP_BULK_STRING RESP_BULK_ERROR RESP_VERB_STRING
}redis_pack_ctx;

void redis_pkfree(redis_pack_ctx *pack);
void redis_udfree(ud_cxt *ud);
//%b:binary - size_t %%:%  C format
char *redis_pack(size_t *size, const char *fmt, ...);
redis_pack_ctx *redis_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *closefd);

#endif//REDIS_H_
