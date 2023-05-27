#include "ltask.h"
#include "argparse.h"

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
    uint32_t nnet = 1;
    uint32_t nworker = 2;
    static const char *const usages[] = {
        "[options] [args]",
        NULL,
    };
    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_GROUP("options"),
        OPT_INTEGER('n', "int", &nnet, "set networker thread number, default 1", NULL, 0, 0),
        OPT_INTEGER('w', "int", &nworker, "set worker thread number, default 2", NULL, 0, 0),
        OPT_END(),
    };
    struct argparse argp;
    argparse_init(&argp, options, usages, 0);
    argparse_describe(&argp, "\nA brief description of what the program does and how it works.",
        "\nAdditional description of the program after the description of the arguments.");
    argparse_parse(&argp, argc, (const char **)argv);

    MEMCHECK();
    unlimit();
    srand((unsigned int)time(NULL));
    mutex_init(&muexit);
    cond_init(&condexit);
    sighandle(on_sigcb, NULL);
    LOGINIT();

    srey_ctx *srey = srey_init(nnet, nworker);
    if (ERR_OK == ltask_startup(srey)) {
        mutex_lock(&muexit);
        cond_wait(&condexit, &muexit);
        mutex_unlock(&muexit);
    }

    srey_free(srey);
    mutex_free(&muexit);
    cond_free(&condexit);
    LOGFREE();
    return 0;
}
