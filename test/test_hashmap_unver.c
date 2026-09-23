// 把 HASHMAP_ITER_VERSIONED 强制成 0，在 64 位主机上也跑一遍游标不带版本号的那一支（32 位构建的默认行为）。
// 这个定义必须排在所有 include 之前，且本文件不 include lib.h，只拿 hashmap.h 与它要的头：
// hashmap 的辅助函数都是 static inline，各编译单元各有一份，与别的文件里按默认值展开的那套互不相干。
// 32 位上默认本来就是 0，游标放不下版本号，反过来强制成 1 做不到
#define HASHMAP_ITER_VERSIONED 0
#include "test_containers.h"
#include "containers/hashmap.h"
#include "utils/utils.h"

#define _UV_N 64
#define _UV_HASH(e) hash_u64((uint64_t)*(e))
#define _UV_CMP(a, b) ((*(a) > *(b)) - (*(a) < *(b)))
HASHMAP_DECL(uv_map, int32_t, _UV_HASH, _UV_CMP)

static int32_t _uv_count(const int32_t *item, void *udata) {
    (void)item;
    (*(int32_t *)udata)++;
    return 1;
}
// 回调里往被扫的表里插一条，scan 须按 version 检出并中止（这道检测与游标带不带版本号无关）
static int32_t _uv_insert(const int32_t *item, void *udata) {
    int32_t k = 1000;
    (void)item;
    uv_map_set((uv_map *)udata, &k);
    return 1;
}

void test_hashmap_iter_unversioned(CuTest *tc) {
    uv_map *m = uv_map_new(0, NULL);
    int32_t seen[_UV_N];
    int32_t *item;
    int32_t k, cnt;
    size_t i;

    CuAssertPtrNotNull(tc, m);
    for (k = 0; k < _UV_N; k++) {
        uv_map_set(m, &k);
    }
    CuAssertIntEquals(tc, _UV_N, (int32_t)uv_map_size(m));

    // 游标就是裸桶下标：每步都不超过桶数；带版本号时高 32 位非 0，这条会先失败
    memset(seen, 0, sizeof(seen));
    i = 0;
    cnt = 0;
    while (uv_map_iter(m, &i, &item)) {
        CuAssertTrue(tc, i >= 1 && i <= m->nbuckets);
        CuAssertTrue(tc, *item >= 0 && *item < _UV_N);
        seen[*item]++;
        cnt++;
    }
    CuAssertIntEquals(tc, _UV_N, cnt);
    for (k = 0; k < _UV_N; k++) {
        CuAssertIntEquals(tc, 1, seen[k]);
    }
    // 走完游标停在 nbuckets，拿旧游标再调也不会从中途接着吐元素
    CuAssertTrue(tc, m->nbuckets == i);
    CuAssertTrue(tc, !uv_map_iter(m, &i, &item));
    CuAssertTrue(tc, !uv_map_iter(m, &i, &item));

    // 原位覆盖不挪位置，持游标覆盖后接着走，总数不变
    i = 0;
    cnt = 0;
    CuAssertTrue(tc, uv_map_iter(m, &i, &item));
    cnt++;
    k = 5;
    CuAssertTrue(tc, NULL != uv_map_set(m, &k));
    while (uv_map_iter(m, &i, &item)) {
        cnt++;
    }
    CuAssertIntEquals(tc, _UV_N, cnt);

    // scan：走完返回 1；回调里增删被检出、返回 0
    cnt = 0;
    CuAssertTrue(tc, uv_map_scan(m, _uv_count, &cnt));
    CuAssertIntEquals(tc, _UV_N, cnt);
    CuAssertTrue(tc, !uv_map_scan(m, _uv_insert, m));
    CuAssertIntEquals(tc, _UV_N + 1, (int32_t)uv_map_size(m));

    // 增删之后游标从 0 重来，新元素在内
    i = 0;
    cnt = 0;
    while (uv_map_iter(m, &i, &item)) {
        cnt++;
    }
    CuAssertIntEquals(tc, _UV_N + 1, cnt);
    uv_map_free(m);
}
