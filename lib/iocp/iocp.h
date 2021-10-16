#ifndef IOCP_H_
#define IOCP_H_

#include "netev.h"
#include "evtype.h"

SREY_NS_BEGIN
#ifdef OS_WIN

class ciocp : public cnetev
{
public:
    ciocp();
    ~ciocp();
    
    bool start();
    void stop();
    struct sockctx *listener(class cchan *pchan,
        const char *phost, const uint16_t &usport, const bool &btcp = true);
    struct sockctx *connectter(class cchan *pchan,
        const char *phost, const uint16_t &usport, const bool &btcp = true);
    struct sockctx *addsock(class cchan *pchan, SOCKET &fd);    
    struct ExPtr *getexfunc()
    {
        return m_exfunc;
    };    
    int32_t _accptex(struct sockctx *plistensock, struct accept_ol *poverlapped);    
    int32_t _recvex(struct sockctx *psock, struct recv_ol *poverlapped);

private:
    void _initexfunc(void);
    void *_getexfunc(const SOCKET &fd, const GUID  *guid);
    struct sockctx *_creatctx(class cchan *pchan,
        const char *phost, const uint16_t &usport, const bool &btcp);
    int32_t _listener(struct sockctx *psock);
    int32_t _udpex(struct sockctx *psock);
    int32_t _accptex(struct sockctx *psock);
    void _freeaccptex(std::vector<struct accept_ol *> &vcsuss);
    int32_t _connectter(struct sockctx *psock);
    int32_t _tcpconnect(struct sockctx *psock);
    int32_t _trybind(struct sockctx *psock);
    int32_t _addsock(struct sockctx *psock);

private:
    int32_t m_threadnum;
    struct ExPtr *m_exfunc;
    class cthread *m_thread;
    class cworker *m_worker;
    HANDLE m_ioport;
};

#endif
SREY_NS_END

#endif//IOCP_H_
