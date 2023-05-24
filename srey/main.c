#include "ltask.h"

#ifdef OS_WIN
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "lib.lib")
    #if WITH_SSL
        #ifdef ARCH_X64
            #pragma comment(lib, "libcrypto_x64.lib")
            #pragma comment(lib, "libssl_x64.lib")
        #else
            #pragma comment(lib, "libcrypto.lib")
            #pragma comment(lib, "libssl.lib")
        #endif
    #endif
#endif

mutex_ctx muexit;
cond_ctx condexit;

static void on_sigcb(int32_t sig, void *arg) {
    PRINT("catch sign: %d", sig);
    cond_signal(&condexit);
}
int main(int argc, char *argv[]) {
    MEMCHECK();
    unlimit();
    srand((unsigned int)time(NULL));
    mutex_init(&muexit);
    cond_init(&condexit);
    sighandle(on_sigcb, NULL);
    LOGINIT();

    srey_ctx *srey = srey_init(2, 2);
    if (ERR_OK == ltask_startup(srey)) {
        mutex_lock(&muexit);
        cond_wait(&condexit, &muexit);
        mutex_unlock(&muexit);
    }

    srey_free(srey);
    LOGFREE();
    mutex_free(&muexit);
    cond_free(&condexit);
    return 0;
}
