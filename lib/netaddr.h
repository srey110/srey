#ifndef NETADDR_H_
#define NETADDR_H_

#include "macro.h"

SREY_NS_BEGIN

class cnetaddr
{
public:
    cnetaddr();
    explicit cnetaddr(const bool &ipv6);
    ~cnetaddr() {};    
    /*
    * \brief          设置地址
    * \param phost    ip
    * \param usport   port
    * \param bipv6    是否IP V6
    * \return         true 成功
    * \return         失敗
    */
    bool setaddr(const char *phost, const uint16_t &usport);
    /*
    * \brief          设置地址
    * \param paddr    struct sockaddr *
    * \return         true 成功
    * \return         失敗
    */
    bool setaddr(const struct sockaddr *paddr);
    /*
    * \brief          获取远端地址信息
    * \param fd       SOCKET
    * \return         true 成功
    * \return         失敗
    */
    bool setreaddr(const SOCKET &fd);
    /*
    * \brief          获取本地地址信息
    * \param fd       SOCKET
    * \return         true 成功
    * \return         失敗
    */
    bool setloaddr(const SOCKET &fd);
    /*
    * \brief          返回地址
    * \return         sockaddr *
    */
    sockaddr *getaddr();
    /*
    * \brief          地址长度
    * \return         地址长度
    */
    socklen_t getsize();
    /*
    * \brief          获取IP
    * \return         ""失敗
    * \return         IP
    */
    std::string getip();
    /*
    * \brief          获取端口
    * \return         端口
    */
    uint16_t getport();
    /*
    * \brief          是否为ipv4
    * \return         true 是
    */
    bool isipv4()
    {
        return IPV4 == m_type;
    };
    /*
    * \brief          是否为ipv6
    * \return         true 是
    */
    bool isipv6()
    {
        return IPV6 == m_type;
    };
    int32_t addrfamily()
    {
        return IPV4 == m_type ? AF_INET : AF_INET6;
    };
    /*
    * \brief          简单检查是否为ipv4，未判断合法
    * \return         true 是
    */
    static bool checkipv4(const char *phost);

private:
    void _clear();    

private:
    enum adrrtype
    {
        IPV4,
        IPV6
    };
    adrrtype m_type;
    sockaddr_in	m_ipv4;
    sockaddr_in6 m_ipv6;
};

SREY_NS_END

#endif//NETADDR_H_
