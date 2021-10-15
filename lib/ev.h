#ifndef EV_H_
#define EV_H_

#include "thread.h"
#include "wot.h"

SREY_NS_BEGIN

class cev : public ctask
{
public:
    cev() {};
    virtual ~cev() {};
    virtual bool init() 
    {
        return false;
    };
    virtual struct sockhandle *listen(class cchan *pchan, const char *phost, const uint64_t &usport)
    {
        return NULL;
    };
    virtual struct sockhandle *connect(class cchan *pchan, const char *phost, const uint64_t &usport)
    {
        return NULL;
    };
    virtual struct sockhandle *addsock(class cchan *pchan, const SOCKET &fd)
    {
        return NULL;
    };
    virtual SOCKET getsock(struct sockhandle *psock)
    {
        return INVALID_SOCK;
    };
    virtual int32_t send(struct sockhandle *psock)
    {
        return -1;
    };

    void beforrun();
    void run();
    void afterrun();
    bool addtimer(class cchan *pchan, const uint32_t &uimsec, const void *pdata)
    {
        return m_wot.add(pchan, uimsec, pdata);
    };

private:
    cwot m_wot;
};

SREY_NS_END

#endif//EV_H_
