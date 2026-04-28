#include "test_base.h"
#include "test_containers.h"
#include "test_crypt.h"
#include "test_utils.h"
#include "test_thread.h"
#include "test_protocol.h"
#include "test_srey.h"
#include "lib.h"

#ifdef OS_WIN
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#endif

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* 基础初始化 */
    sock_init();
    unlimit();
    log_init(NULL);

    /* ── 层 1：纯内存单元测试套件 ── */
    CuString *output = CuStringNew();
    CuSuite  *suite  = CuSuiteNew();

    test_base(suite);        /* 内存宏、原子操作 */
    test_containers(suite);  /* mspc、hashmap、heap、queue、sarray */
    test_crypt(suite);       /* base64、crc、digest、hmac、urlraw、xor */
    test_utils(suite);       /* pack/unpack、binary、buffer、sfid、hash_ring、netaddr */
    test_thread(suite);      /* mutex、spinlock、rwlock、cond、thread */
    test_protocol(suite);    /* HTTP、Redis RESP、URL 解析、custz */

    CuSuiteRun(suite);
    CuSuiteSummary(suite, output);
    CuSuiteDetails(suite, output);
    printf("%s\n", output->buffer);

    int unit_failed = suite->failCount;

    CuStringDelete(output);
    CuSuiteDelete(suite);

    /* ── 层 2：srey 集成测试（需要 loader + 网络事件循环）── */
    int intg_failed = test_srey();

    _memcheck();
    sock_clean();
    log_free();
    
    /* 任一层有失败则返回非零，便于 CI 捕获 */
    return (unit_failed > 0 || intg_failed > 0) ? 1 : 0;
}
