#include "test_lock.h"
#include "thread.h"
#include "mutex.h"
#include "lockguard.h"
#include "rwlock.h"
#include "spinlock.h"
#include "utils.h"

using namespace SREY_NS;

uint32_t inum = INIT_NUMBER;
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
int32_t ilp = INIT_NUMBER;
cmutex mu;
void mulock(void *pparam)
{
    for (int32_t i = 0; i < ilp; i++)
    {
        clockguard<cmutex> lg(mu);
        inum++;
    }
}
void ctest_lock::_testlock(void(*lockfunc)(void *pparam))
{
    inum = INIT_NUMBER;
    ilp = 1000000;
    cthread st1;
    cthread st2;
    cthread st3;
    st1.creat(lockfunc, NULL);
    st2.creat(lockfunc, NULL);
    st3.creat(lockfunc, NULL);
    st1.join();
    st2.join();
    st3.join();
}
void ctest_lock::test_mulock(void)
{
    _testlock(mulock);
    CPPUNIT_ASSERT(inum == ilp * 3);
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
    _testlock(mutrylock);
    CPPUNIT_ASSERT(inum == ilp * 3);
}
cspinlock splock;
void spinlock(void *pparam)
{
    for (int32_t i = 0; i < ilp; i++)
    {
        clockguard<cspinlock> lg(splock);
        inum++;
    }
}
void ctest_lock::test_splock(void)
{
    _testlock(spinlock);
    CPPUNIT_ASSERT(inum == ilp * 3);
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
    _testlock(tryspinlock);
    CPPUNIT_ASSERT(inum == ilp * 3);
}
