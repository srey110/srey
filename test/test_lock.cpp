#include "test_lock.h"

using namespace SREY_NS;

uint32_t itmp = INIT_NUMBER;
uint32_t inum = INIT_NUMBER;
int32_t ilp = INIT_NUMBER;
cmutex mu;
crwlock rwlock;

void ctest_lock::test_atomic(void)
{
    inum = INIT_NUMBER;
    ATOMIC_ADD(&inum, 1);
    CPPUNIT_ASSERT(inum == 1);
    ATOMIC_SET(&inum, 3);
    CPPUNIT_ASSERT(inum == 3);
    int32_t itmp = ATOMIC_GET(&inum);
    CPPUNIT_ASSERT(itmp == 3);
    itmp = ATOMIC_CAS(&inum, 3, 4);
    CPPUNIT_ASSERT(itmp == 3 && inum == 4);
}
void ctest_lock::_testlock(void(*lockfunc)(void *pparam))
{
    inum = INIT_NUMBER;
    ilp = 10000;
    cthread st1;
    cthread st2;
    cthread st3;
    st1.creat(lockfunc, NULL);
    st1.waitstart();
    st2.creat(lockfunc, NULL);
    st2.waitstart();
    st3.creat(lockfunc, NULL);
    st3.waitstart();
    st1.join();
    st2.join();
    st3.join();
}
void mulock(void *pparam)
{
    for (int32_t i = 0; i < ilp; i++)
    {
        clockguard<cmutex> lg(&mu);
        inum++;
    }
}
void ctest_lock::test_mulock(void)
{
    PRINTF("%s", "test_mulock");
    _testlock(mulock);
    CPPUNIT_ASSERT((int32_t)inum == ilp * 3);
}
void mutrylock(void *pparam)
{
    for (int32_t i = 0; i < ilp;)
    {
        if (mu.trylock())
        {
            inum++;
            mu.unlock();
            i++;
        }        
    }
}
void ctest_lock::test_mutrylock(void)
{    
    PRINTF("%s", "test_mutrylock");
    _testlock(mutrylock);
    CPPUNIT_ASSERT((int32_t)inum == ilp * 3);
}
cspinlock splock;
void spinlock(void *pparam)
{
    for (int32_t i = 0; i < ilp; i++)
    {
        clockguard<cspinlock> lg(&splock);
        inum++;
    }
}
void ctest_lock::test_splock(void)
{
    PRINTF("%s", "test_splock");
    _testlock(spinlock);
    CPPUNIT_ASSERT((int32_t)inum == ilp * 3);
}
void tryspinlock(void *pparam)
{
    for (int32_t i = 0; i < ilp;)
    {
        if (splock.trylock())
        {
            inum++;
            splock.unlock();

            i++;
        }
    }
}
void ctest_lock::test_sptrylock(void)
{
    PRINTF("%s", "test_sptrylock");
    _testlock(tryspinlock);
    CPPUNIT_ASSERT((int32_t)inum == ilp * 3);
}
void rwlock_w(void *pparam)
{
    for (int32_t i = 0; i < ilp;)
    {
        rwlock.wrlock();
        if (itmp == INIT_NUMBER)
        {
            inum++;
            itmp = inum;
            i++;
        }
        rwlock.unlock();
    }
}
void rwlock_r(void *pparam)
{
    for (int32_t i = 0; i < ilp * 2; )
    {
        rwlock.rdlock();
        if (itmp != INIT_NUMBER)
        {
            if (i != ilp * 2 - 1)
            {
                itmp = INIT_NUMBER;
            }
            i++;
        }
        rwlock.unlock();
    }
}
void ctest_lock::_testrwlock(void(*wfunc)(void *pparam), void(*rfunc)(void *pparam))
{
    itmp = INIT_NUMBER;
    inum = INIT_NUMBER;
    ilp = 1000;
    cthread st1;
    cthread st2;
    cthread st3;
    st1.creat(wfunc, NULL);
    st1.waitstart();
    st2.creat(wfunc, NULL);
    st2.waitstart();
    st3.creat(rfunc, NULL);
    st3.waitstart();
    st1.join();
    st2.join();
    st3.join();    
}
void ctest_lock::test_rwlock(void)
{
    PRINTF("%s", "test_rwlock");
    _testrwlock(rwlock_w, rwlock_r);
    CPPUNIT_ASSERT((int32_t)inum == ilp * 2 && itmp == inum);
}

void tryrwlock_w(void *pparam)
{
    for (int32_t i = 0; i < ilp;)
    {
        if (rwlock.trywrlock())
        {
            if (itmp == INIT_NUMBER)
            {
                inum++;
                itmp = inum;
                i++;
            }
            rwlock.unlock();
        }        
    }
}
void tryrwlock_r(void *pparam)
{
    for (int32_t i = 0; i < ilp * 2; )
    {
        if (rwlock.tryrdlock())
        {
            if (itmp != INIT_NUMBER)
            {
                if (i != ilp * 2 - 1)
                {
                    itmp = INIT_NUMBER;
                }
                i++;
            }
            rwlock.unlock();
        }
    }
}
void ctest_lock::test_tryrwlock(void)
{
    PRINTF("%s", "test_tryrwlock");
    _testrwlock(tryrwlock_w, tryrwlock_r);
    CPPUNIT_ASSERT((int32_t)inum == ilp * 2 && itmp == inum);
}
