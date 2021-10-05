#include "test_chan.h"
#include "errcode.h"

#define CAPSIZE 5

bool bexit = false;
int32_t iloop = INIT_NUMBER;
int32_t ich1 = INIT_NUMBER;
int32_t ich2 = INIT_NUMBER;
int32_t ich3 = INIT_NUMBER;
int32_t ich4 = INIT_NUMBER;

class creadchan : public ctask
{
public:
    creadchan(cchan **pchan) : pchans(pchan) {};
    ~creadchan() {};

    void beforrun()
    {
        ich1 = INIT_NUMBER;
        ich2 = INIT_NUMBER;
        ich3 = INIT_NUMBER;
        ich4 = INIT_NUMBER;
        PRINTF("%s", "run read chan thread.");
    };
    void run() 
    {        
        while (!bexit)
        {
            if (pchans[0]->canrecv())
            {
                ich1++;
                int32_t i;
                pchans[0]->recv(&i);
            }
            if (pchans[1]->canrecv())
            {
                ich2++;
                int64_t i;
                pchans[1]->recv(&i);
            }
            if (pchans[2]->canrecv())
            {
                ich3++;
                double d;
                pchans[2]->recv(&d);
            }
            if (pchans[3]->canrecv())
            {
                ich4++;
                std::string str;
                pchans[3]->recv(&str);
            }
        }
    };
    void afterrun()
    {
        PRINTF("%s", "read chan thread finish.");
    };

private:
    cchan **pchans;
};

class cselrcchan : public ctask
{
public:
    cselrcchan(cchan **pchan) : pchans(pchan) {};
    ~cselrcchan() {};

    void beforrun()
    {
        ich1 = INIT_NUMBER;
        ich2 = INIT_NUMBER;
        ich3 = INIT_NUMBER;
        ich4 = INIT_NUMBER;
        PRINTF("%s", "run select read chan thread.");
    };
    void run()
    {
        while (!bexit)
        {
            void *pval = NULL;
            int32_t index = cchan::select(pchans, 4, &pval, NULL, 0, NULL);
            if (NULL != pval)
            {
                switch (index)
                {
                case 0:
                {
                    ich1++;
                    int32_t *p = (int32_t *)pval;
                    SAFE_DEL(p);
                }
                break;
                case 1:
                {
                    ich2++;
                    int64_t *p = (int64_t *)pval;
                    SAFE_DEL(p);
                }
                break;
                case 2:
                {
                    ich3++;
                    double *p = (double *)pval;
                    SAFE_DEL(p);
                }
                break;
                case 3:
                {
                    ich4++;
                    std::string *p = (std::string *)pval;
                    SAFE_DEL(p);
                }
                break;
                default:
                    break;
                }
            }
        }
    };
    void afterrun()
    {
        PRINTF("%s", "select read chan thread finish.");
    };

private:
    cchan **pchans;
};

void write_cb(void *pparam)
{
    char acbuf[64];
    cchan **pchans = (cchan **)pparam;
    PRINTF("%s", "run write chan thread.");
    for (int32_t i = 0; i < iloop; i++)
    {
        pchans[0]->send(i);
        int64_t ui = 15800000 + i;
        pchans[1]->send(ui);
        double d = 3.14 + i;
        pchans[2]->send(d);
        ZERO(acbuf, sizeof(acbuf));
        SNPRINTF(acbuf, sizeof(acbuf) - 1, "test send buff, %d", i);
        pchans[3]->send(acbuf, sizeof(acbuf));
    }
    PRINTF("%s", "write chan thread finish.");
}
void selwr_cb(void *pparam)
{
    char acbuf[64];
    cchan **pchans = (cchan **)pparam;
    cchan *ptmpch[4];
    void *sendmsg[4];
    int32_t icount = INIT_NUMBER;
    int32_t index = INIT_NUMBER;
    PRINTF("%s", "run select write chan thread.");
    for (int32_t i = 0; i < iloop; i++)
    {
        for (int32_t k = 0; k < 4; k++)
        {
            ptmpch[k] = pchans[k];
        }

        int32_t *p32 = new(std::nothrow) int32_t();
        *p32 = i;
        sendmsg[0] = p32;
        int64_t *p64 = new(std::nothrow) int64_t();
        *p64 = 15800000 + i;
        sendmsg[1] = p64;
        double *pd = new(std::nothrow) double();
        *pd = 3.14 + i;
        sendmsg[2] = pd;
        std::string *pbuf = new(std::nothrow) std::string();
        SNPRINTF(acbuf, sizeof(acbuf) - 1, "test send buff, %d", i);
        *pbuf = acbuf;
        sendmsg[3] = pbuf;

        icount = INIT_NUMBER;
        while (icount < 4)
        {
            index = cchan::select(NULL, 0, NULL, ptmpch, 4 - icount, sendmsg);
            if (ERR_FAILED != index)
            {
                for (int32_t k = index; k < 4 - 1; k++)
                {
                    ptmpch[k] = ptmpch[k + 1];
                    sendmsg[k] = sendmsg[k + 1];
                }

                icount++;
            }
        }
    }
    PRINTF("%s", "select write chan thread finish.");
}
void ctest_chan::_test_chan(int32_t icap, bool bselect)
{
    bexit = false;
    pth1 = new(std::nothrow) cthread();
    pth2 = new(std::nothrow) cthread();
    cchan **pchans = new(std::nothrow) cchan *[4];
    for (int32_t i = 0; i < 4; i++)
    {
        pchans[i] = new(std::nothrow) cchan(icap);
    }

    creadchan readtask(pchans);
    cselrcchan seltask(pchans);
    if (bselect)
    {
        pth1->creat(selwr_cb, pchans);
        pth1->waitstart();

        pth2->creat(&seltask);
        pth2->waitstart();
    }
    else
    {
        pth1->creat(write_cb, pchans);
        pth1->waitstart();

        pth2->creat(&readtask);
        pth2->waitstart();
    }
    
    pth1->join();
    
    bool bempty = true;
    while (true)
    {
        bempty = true;
        for (int32_t i = 0; i < 4; i++)
        {
            if (pchans[i]->size() != INIT_NUMBER)
            {
                bempty = false;
                break;
            }
        }
        if (bempty)
        {
            bexit = true;
            break;
        }
        else
        {
            USLEEP(10);
        }
    }

    pth2->join();    
    for (int32_t i = 0; i < 4; i++)
    {
        pchans[i]->close();
    }

    for (int32_t i = 0; i < 4; i++)
    {
        SAFE_DEL(pchans[i]);
    }
    SAFE_DELARR(pchans);
    SAFE_DEL(pth1);
    SAFE_DEL(pth2);
}
void ctest_chan::test_buffchan(void)
{
    iloop = 10000;
    _test_chan(CAPSIZE, false);
    CPPUNIT_ASSERT(iloop == ich1 && iloop == ich2
        && iloop == ich3 && iloop == ich4);
}
void ctest_chan::test_unbuffchan(void)
{
    iloop = 10000;
    _test_chan(INIT_NUMBER, false);
    CPPUNIT_ASSERT(iloop == ich1 && iloop == ich2 
        && iloop == ich3 && iloop == ich4);
}
void ctest_chan::test_bufselect(void)
{
    iloop = 10000;
    _test_chan(CAPSIZE, true);
    CPPUNIT_ASSERT(iloop == ich1 && iloop == ich2 
        && iloop == ich3 && iloop == ich4);
}
