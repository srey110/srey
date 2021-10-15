#ifndef NETADDR_H_
#define NETADDR_H_

#include "macro.h"

SREY_NS_BEGIN

class cnetaddr
{
public:
    cnetaddr() {};
    ~cnetaddr() {};
    /*
    * \brief        设置sockaddr_in
    * \param phost  ip
    * \param port   port
    * \return       true 成功
    */
    bool setaddr(const char *ip, const uint16_t &port);
    /*
    * \brief        设置sockaddr_in
    * \param paddr  sockaddr
    * \return       true 成功
    */
    void setaddr(const struct sockaddr *paddr);
    /*
    * \brief        根据socket句柄获取远端地址信息
    * \param fd     SOCKET
    * \return       true 成功
    */
    bool setremaddr(const SOCKET &fd);
    /*
    * \brief        根据socket句柄获取本地地址信息
    * \param fd     SOCKET
    * \return       true 成功
    */
    bool setlocaddr(const SOCKET &fd);
    /*
    * \brief        获取sockaddr
    * \return       sockaddr
    */
    sockaddr *getaddr();
    /*
    * \brief        获取地址长度
    * \return       长度
    */
    size_t getsize();
    /*
    * \brief        获取IP
    * \return       ip
    */
    std::string getip();
    /*
    * \brief        获取端口
    * \return       端口
    */
    uint16_t getport();

private:
    sockaddr_in	m_ipv4;
};

SREY_NS_END

#endif//NETADDR_H_
