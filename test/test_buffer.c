#include "test_buffer.h"

void test_buffer()
{
    const char *pbuf0 = "321";
    const char *pbuf1 = "1234567890";
    const char *pbuf2 = "0987654321";
    const char *pbuf3 = "abcdefdhijklmnopqrst";
    const char *pbuf4 = "ABCDEFDHIJKLMNOPQRST";
    buffer_ctx stbuf;
    buffer_init(&stbuf);

    //11 22 33 44 55 66
    buffer_append(&stbuf, "11", 2);
    buffer_append(&stbuf, "22", 2);
    buffer_append(&stbuf, "23", 2);
    buffer_append(&stbuf, "44", 2);
    buffer_append(&stbuf, "55", 2);
    buffer_append(&stbuf, "66", 2);

    int32_t ipos = buffer_search(&stbuf, 0, "2344556", strlen("2344556"));

    buffer_appendv(&stbuf, pbuf1, strlen(pbuf1));
    buffer_appendv(&stbuf, pbuf2, strlen(pbuf2));

    size_t uilens = strlen(pbuf1);
    int32_t irtn = buffer_append(&stbuf, (void*)pbuf1, uilens);
    TEST_ASSERT(irtn == ERR_OK);

    //irtn = buffer_appendv(&stbuf, "%s", pbuf3);

    uilens = strlen(pbuf3);
    irtn = buffer_append(&stbuf, (void*)pbuf3, uilens);
    TEST_ASSERT(irtn == ERR_OK);

    char actmp[128] = { 0 };
    buffer_remove(&stbuf, actmp, buffer_size(&stbuf));

    uilens = strlen(pbuf0);
    irtn = buffer_append(&stbuf, (void*)pbuf0, uilens);
    
    IOV_TYPE piov[2];
    _buffer_expand_iov(&stbuf, uilens, piov, 2);

    uilens = strlen(pbuf4);
    irtn = buffer_append(&stbuf, (void*)pbuf4, uilens);

    buffer_free(&stbuf);
}
