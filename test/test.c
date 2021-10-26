#include "test_thread.h"
#include "test_buffer.h"

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif

int main(int argc, char *argv[])
{
    //test_buffer();
    test_thread();

    return 0;
}
