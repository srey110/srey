#include "task_priority.h"

typedef struct task_priority_args {
    int32_t *ok;
}task_priority_args;

// ── 测试 1: round-trip 边界值 setter→getter 一致 ──────────────────────────
static int32_t _test_roundtrip(task_ctx *task) {
    int32_t cases[] = { 0, 1, TASK_PRIORITY_MAX / 2, TASK_PRIORITY_MAX };
    int32_t got;
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        task_set_priority(task, cases[i]);
        got = task_get_priority(task);
        if (got != cases[i]) {
            LOG_ERROR("priority roundtrip: set %d, got %d.", cases[i], got);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}

// ── 测试 2: clamp 上限到 TASK_PRIORITY_MAX ────────────────────────────────
static int32_t _test_clamp_upper(task_ctx *task) {
    int32_t cases[] = { TASK_PRIORITY_MAX + 1, TASK_PRIORITY_MAX + 100, INT32_MAX };
    int32_t got;
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        task_set_priority(task, cases[i]);
        got = task_get_priority(task);
        if (got != TASK_PRIORITY_MAX) {
            LOG_ERROR("priority clamp upper: set %d, expect %d, got %d.",
                      cases[i], TASK_PRIORITY_MAX, got);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}

// ── 测试 3: clamp 下限到 0 ────────────────────────────────────────────────
static int32_t _test_clamp_lower(task_ctx *task) {
    int32_t cases[] = { -1, -100, INT32_MIN };
    int32_t got;
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        task_set_priority(task, cases[i]);
        got = task_get_priority(task);
        if (0 != got) {
            LOG_ERROR("priority clamp lower: set %d, expect 0, got %d.", cases[i], got);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}

// ── 测试 4: 全范围循环值始终落在 [0, TASK_PRIORITY_MAX] ──────────────────
static int32_t _test_range(task_ctx *task) {
    int32_t got;
    for (int32_t i = -100; i <= TASK_PRIORITY_MAX + 100; i++) {
        task_set_priority(task, i);
        got = task_get_priority(task);
        if (got < 0 || got > TASK_PRIORITY_MAX) {
            LOG_ERROR("priority range: set %d, got %d out of [0,%d].",
                      i, got, TASK_PRIORITY_MAX);
            return ERR_FAILED;
        }
    }
    // 收尾恢复默认值,避免污染后续测试
    task_set_priority(task, 0);
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_priority_args *arg = (task_priority_args *)coro_get_arg(task);
    if (ERR_OK != _test_roundtrip(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_clamp_upper(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_clamp_lower(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_range(task)) {
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("priority tested.");
}

void task_priority_start(loader_ctx *loader, const char *name, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    task_priority_args *arg;
    CALLOC(arg, 1, sizeof(task_priority_args));
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
