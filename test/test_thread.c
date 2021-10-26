#include "test_thread.h"

void test_fmterror(void)
{
#ifdef OS_WIN
    TEST_ASSERT(strcmp(_fmterror(10022), "An invalid argument was supplied.\r\n") == 0);
#endif
}
void test_atomic(void)
{
    volatile atomic_t ui32 = 0;
    ATOMIC_ADD(&ui32, 1);
    TEST_ASSERT(ui32 == 1);

    ATOMIC_SET(&ui32, 3);
    TEST_ASSERT(ui32 == 3);

    atomic_t itmp = ATOMIC_GET(&ui32);
    TEST_ASSERT(itmp == 3);

    itmp = 0;
    itmp = ATOMIC_CAS(&ui32, 3, 4);
    TEST_ASSERT(itmp);

    itmp = 0;
    itmp = ATOMIC_CAS(&ui32, 5, 4);
    TEST_ASSERT(!itmp);
}
uint32_t itmp = 0;
uint32_t inum = 0;
int32_t ilp = 0;
int32_t iwaitcount = 0;
void _testlock(void(*lockfunc)(void *, void*, void*), void *param1)
{
    iwaitcount = 0;
    inum = 0;
    ilp = 10000;
    thread_ctx th1;
    thread_init(&th1);
    thread_ctx th2;
    thread_init(&th2);
    thread_ctx th3;
    thread_init(&th3);
    cond_ctx cond;
    cond_init(&cond);
    mutex_ctx condmu;
    mutex_init(&condmu);

    thread_creat(&th1, lockfunc, param1, (void*)&cond, (void*)&condmu);
    thread_waitstart(&th1);
    thread_creat(&th2, lockfunc, param1, (void*)&cond, (void*)&condmu);
    thread_waitstart(&th2);
    thread_creat(&th3, lockfunc, param1, (void*)&cond, (void*)&condmu);
    thread_waitstart(&th3);

    while (iwaitcount != 3);
    cond_broadcast(&cond);

    thread_join(&th1);
    thread_join(&th2);
    thread_join(&th3);

    cond_free(&cond);
    mutex_free(&condmu);
}
void mulock(void *pparam1, void *pparam2, void *pparam3)
{
    mutex_ctx *pmu = (mutex_ctx *)pparam1;
    cond_ctx *pcond = (cond_ctx *)pparam2;
    mutex_ctx *pcondmu = (mutex_ctx *)pparam3;

    mutex_lock(pcondmu);
    iwaitcount++;
    cond_wait(pcond, pcondmu);
    iwaitcount--;
    mutex_unlock(pcondmu);
    for (int32_t i = 0; i < ilp; i++)
    {
        mutex_lock(pmu);
        inum++;
        mutex_unlock(pmu);
    }
}
void test_mulock(void)
{
    PRINTF("%s", "test_mulock, 3 thread");
    mutex_ctx mu;
    mutex_init(&mu);
    _testlock(mulock, (void*)&mu);
    mutex_free(&mu);
    TEST_ASSERT((int32_t)inum == ilp * 3);
}
void mutrylock(void *pparam1, void *pparam2, void *pparam3)
{
    mutex_ctx *pmu = (mutex_ctx *)pparam1;
    cond_ctx *pcond = (cond_ctx *)pparam2;
    mutex_ctx *pcondmu = (mutex_ctx *)pparam3;

    mutex_lock(pcondmu);
    iwaitcount++;
    cond_wait(pcond, pcondmu);
    iwaitcount--;
    mutex_unlock(pcondmu);
    for (int32_t i = 0; i < ilp;)
    {
        if (ERR_OK == mutex_trylock(pmu))
        {
            inum++;
            mutex_unlock(pmu);
            i++;
        }
    }
}
void test_mutrylock(void)
{
    PRINTF("%s", "test_mutrylock, 3 thread");
    mutex_ctx mu;
    mutex_init(&mu);
    _testlock(mutrylock, (void*)&mu);
    mutex_free(&mu);
    TEST_ASSERT((int32_t)inum == ilp * 3);
}
void spinlock(void *pparam1, void *pparam2, void *pparam3)
{
    spin_ctx *pspin = (spin_ctx *)pparam1;
    cond_ctx *pcond = (cond_ctx *)pparam2;
    mutex_ctx *pcondmu = (mutex_ctx *)pparam3;

    mutex_lock(pcondmu);
    iwaitcount++;
    cond_wait(pcond, pcondmu);
    iwaitcount--;
    mutex_unlock(pcondmu);
    for (int32_t i = 0; i < ilp; i++)
    {
        spin_lock(pspin);
        inum++;
        spin_unlock(pspin);
    }
}
void test_splock(void)
{
    PRINTF("%s", "test_spinlock, 3 thread");
    spin_ctx spin;
    spin_init(&spin, ONEK);
    _testlock(spinlock, (void*)&spin);
    spin_free(&spin);
    TEST_ASSERT((int32_t)inum == ilp * 3);
}
void spintrylock(void *pparam1, void *pparam2, void *pparam3)
{
    spin_ctx *pspin = (spin_ctx *)pparam1;
    cond_ctx *pcond = (cond_ctx *)pparam2;
    mutex_ctx *pcondmu = (mutex_ctx *)pparam3;

    mutex_lock(pcondmu);
    iwaitcount++;
    cond_wait(pcond, pcondmu);
    iwaitcount--;
    mutex_unlock(pcondmu);
    for (int32_t i = 0; i < ilp;)
    {
        if (ERR_OK == spin_trylock(pspin))
        {
            inum++;
            spin_unlock(pspin);
            i++;
        }
    }
}
void test_sptrylock(void)
{
    PRINTF("%s", "test_spintrylock, 3 thread");
    spin_ctx spin;
    spin_init(&spin, ONEK);
    _testlock(spintrylock, (void*)&spin);
    spin_free(&spin);
    TEST_ASSERT((int32_t)inum == ilp * 3);
}
void _testrwlock(void(*wfunc)(void *, void*, void*),
    void(*rfunc)(void *, void*, void*), void *param)
{
    iwaitcount = 0;
    inum = 0;
    itmp = 0;
    ilp = 1000;
    thread_ctx th1;
    thread_init(&th1);
    thread_ctx th2;
    thread_init(&th2);
    thread_ctx th3;
    thread_init(&th3);
    cond_ctx cond;
    cond_init(&cond);
    mutex_ctx condmu;
    mutex_init(&condmu);

    thread_creat(&th1, wfunc, param, (void*)&cond, (void*)&condmu);
    thread_waitstart(&th1);
    thread_creat(&th2, wfunc, param, (void*)&cond, (void*)&condmu);
    thread_waitstart(&th2);
    thread_creat(&th3, rfunc, param, (void*)&cond, (void*)&condmu);
    thread_waitstart(&th3);

    while (iwaitcount != 3);
    cond_broadcast(&cond);

    thread_join(&th1);
    thread_join(&th2);
    thread_join(&th3);

    cond_free(&cond);
    mutex_free(&condmu);
}
void rwlock_w(void *pparam1, void *pparam2, void *pparam3)
{
    rwlock_ctx *prwlock = (rwlock_ctx *)pparam1;
    cond_ctx *pcond = (cond_ctx *)pparam2;
    mutex_ctx *pcondmu = (mutex_ctx *)pparam3;

    mutex_lock(pcondmu);
    iwaitcount++;
    cond_wait(pcond, pcondmu);
    iwaitcount--;
    mutex_unlock(pcondmu);
    for (int32_t i = 0; i < ilp;)
    {
        rwlock_wrlock(prwlock);
        if (itmp == 0)
        {
            inum++;
            itmp = inum;
            i++;
        }
        rwlock_unlock(prwlock);
    }
}
void rwlock_r(void *pparam1, void *pparam2, void *pparam3)
{
    rwlock_ctx *prwlock = (rwlock_ctx *)pparam1;
    cond_ctx *pcond = (cond_ctx *)pparam2;
    mutex_ctx *pcondmu = (mutex_ctx *)pparam3;

    mutex_lock(pcondmu);
    iwaitcount++;
    cond_wait(pcond, pcondmu);
    iwaitcount--;
    mutex_unlock(pcondmu);
    for (int32_t i = 0; i < ilp * 2; )
    {
        rwlock_rdlock(prwlock);
        if (itmp != 0)
        {
            if (i != ilp * 2 - 1)
            {
                itmp = 0;
            }
            i++;
        }
        rwlock_unlock(prwlock);
    }
}
void test_rwlock(void)
{
    PRINTF("%s", "test_rwlock, 2 thread write, 1 thread read");
    rwlock_ctx rwlock;
    rwlock_init(&rwlock);
    _testrwlock(rwlock_w, rwlock_r, (void*)&rwlock);
    rwlock_free(&rwlock);
    TEST_ASSERT((int32_t)inum == ilp * 2 && itmp == inum);
}
void rwlock_tryw(void *pparam1, void *pparam2, void *pparam3)
{
    rwlock_ctx *prwlock = (rwlock_ctx *)pparam1;
    cond_ctx *pcond = (cond_ctx *)pparam2;
    mutex_ctx *pcondmu = (mutex_ctx *)pparam3;

    mutex_lock(pcondmu);
    iwaitcount++;
    cond_wait(pcond, pcondmu);
    iwaitcount--;
    mutex_unlock(pcondmu);
    for (int32_t i = 0; i < ilp;)
    {
        if (ERR_OK == rwlock_trywrlock(prwlock))
        {
            if (itmp == 0)
            {
                inum++;
                itmp = inum;
                i++;
            }
            rwlock_unlock(prwlock);
        }
    }
}
void rwlock_tryr(void *pparam1, void *pparam2, void *pparam3)
{
    rwlock_ctx *prwlock = (rwlock_ctx *)pparam1;
    cond_ctx *pcond = (cond_ctx *)pparam2;
    mutex_ctx *pcondmu = (mutex_ctx *)pparam3;

    mutex_lock(pcondmu);
    iwaitcount++;
    cond_wait(pcond, pcondmu);
    iwaitcount--;
    mutex_unlock(pcondmu);
    for (int32_t i = 0; i < ilp * 2; )
    {
        if (ERR_OK == rwlock_tryrdlock(prwlock))
        {
            if (itmp != 0)
            {
                if (i != ilp * 2 - 1)
                {
                    itmp = 0;
                }
                i++;
            }
            rwlock_unlock(prwlock);
        }
    }
}
void test_tryrwlock(void)
{
    PRINTF("%s", "test_tryrwlock, 2 thread write, 1 thread read");
    rwlock_ctx rwlock;
    rwlock_init(&rwlock);
    _testrwlock(rwlock_tryw, rwlock_tryr, (void*)&rwlock);
    rwlock_free(&rwlock);
    TEST_ASSERT((int32_t)inum == ilp * 2 && itmp == inum);
}

void test_thread(void)
{
    //test_fmterror();
    test_atomic();
    //test_mulock();
    //test_mutrylock();
    //test_splock();
    //test_sptrylock();
    //test_rwlock();
    //test_tryrwlock();
}
