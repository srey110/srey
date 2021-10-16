#ifndef EV_H_
#define EV_H_

#include "thread.h"
#include "chan.h"
#include "mutex.h"
#include "chainbuffer.h"
#include "netaddr.h"

SREY_NS_BEGIN

struct sockctx
{
    SOCKET sock;
    int32_t socktype;//SOCK_STREAM  SOCK_DGRAM;
    int32_t ipproto; //IPPROTO_TCP IPPROTO_UDP
    class cchan *chan;
    cnetaddr addr;
    cchainbuffer buffer;
    cmutex mutex;
    bool chansend(void *pdata)
    {
        mutex.lock();
        bool bok = chan->send(pdata);
        mutex.unlock();

        return bok;
    }
    void setchan(class cchan *pchan)
    {
        mutex.lock();
        chan = pchan;
        mutex.unlock();
    };
};

class cnetev
{
public:
    cnetev()
    {};
    ~cnetev() {};
    virtual bool start() { return true; };
    virtual void stop() {};
    /*
    * \brief          更换接收消息cchan, 该socket 所有消息都发送到此chan
    * \param psock    socket相关
    * \param pchan    cchan
    */
    void changechan(struct sockctx *psock, class cchan *pchan) 
    {
        psock->setchan(pchan);
    };
    /*
    * \brief          新建一监听
    * \param pchan    cchan 接收消息 
    * \param phost    ip
    * \param usport   port
    * \param btcp     是否为TCP
    * \return         sockhandle
    */
    virtual struct sockctx *listener(class cchan *pchan,
        const char *phost, const uint16_t &usport, const bool &btcp = true)
    {
        return NULL;
    };
    /*
    * \brief          新建一连接
    * \param pchan    接收消息的cchan
    * \param phost    ip
    * \param usport   port
    * \param btcp     是否为TCP
    * \return         sockhandle
    */
    virtual struct sockctx *connectter(class cchan *pchan,
        const char *phost, const uint16_t &usport, const bool &btcp = true)
    {
        return NULL;
    };
    /*
    * \brief          将已有链接加入事件管理
    * \param pchan    接收消息的cchan
    * \param fd       SOCKET
    * \return         sockhandle
    */
    virtual struct sockctx *addsock(class cchan *pchan, SOCKET &fd)
    {
        return NULL;
    };
    SOCKET getsock(struct sockctx *psock)
    {
        return psock->sock;
    };
    virtual int32_t send(struct sockctx *psock)
    {
        return -1;
    };  
};

SREY_NS_END

#endif//EV_H_
