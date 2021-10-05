#include "utils.h"
#include "netaddr.h"
#include "buffer.h"
#include "errcode.h"

SREY_NS_BEGIN

uint64_t ntohl64(const uint64_t &ulval)
{
	//大小端
	static union
	{
		char a[4];
        uint32_t ul;
	}endian = { { 'L', '?', '?', 'B' } };
	#define ENDIAN ((char)endian.ul) 

	if ('L' == ENDIAN)
	{
		uint64_t uiret = INIT_NUMBER;
        uint32_t ulhigh, ullow;

		ullow = ulval & 0xFFFFFFFF;
		ulhigh = (ulval >> 32) & 0xFFFFFFFF;

		ullow = ntohl(ullow);
		ulhigh = ntohl(ulhigh);

        uiret = ullow;
        uiret <<= 32;
        uiret |= ulhigh;

		return uiret;
	}

	return ulval;
}
uint32_t threadid()
{
#ifdef OS_WIN
	return (uint32_t)GetCurrentThreadId();
#else
	return (uint32_t)pthread_self();
#endif
}
uint32_t procsnum()
{
#ifdef OS_WIN
	SYSTEM_INFO stinfo;
	GetSystemInfo(&stinfo);

	return (uint32_t)stinfo.dwNumberOfProcessors;
#else
	return (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
#endif
}
bool fileexist(const char *pname)
{
    return ERR_OK == ACCESS(pname, 0);
}
bool isfile(const char *pname)
{
    struct STAT st;
    if (ERR_OK != STAT(pname, &st))
    {
        return false;
    }
#ifdef OS_WIN
    if (_S_IFREG & st.st_mode)
    {
        return true;
    }
#else
    if (S_ISREG & st.st_mode)
    {
        return true;
    }
#endif
    return false;
}
bool isdir(const char *pname)
{
    struct STAT st;
    if (ERR_OK != STAT(pname, &st))
    {
        return false;
    }
#ifdef OS_WIN
    if (_S_IFDIR & st.st_mode)
    {
        return true;
    }
#else
    if (S_ISDIR & st.st_mode)
    {
        return true;
    }
#endif
    return false;
}
int64_t filesize(const char *pname)
{
    struct STAT st;
    if (ERR_OK != STAT(pname, &st))
    {
        return ERR_FAILED;
    }

    return st.st_size;
}
std::string dirnam(const char *path)
{
#ifdef OS_WIN
    std::string strpath(path);
    size_t iPos = strpath.find_last_of(PATH_SEPARATOR);
    if (std::string::npos != iPos)
    {
        strpath = strpath.substr(0, iPos);
    }

    return strpath;
#else
    char actmp[PATH_LENS] = { 0 };
    memcpy(actmp, path, strlen(path));
    return dirname(actmp);
#endif 
}
std::string getpath()
{
    int32_t isize = INIT_NUMBER;
    char path[PATH_LENS] = { 0 };

#ifdef OS_WIN 
    isize = (int32_t)GetModuleFileName(NULL, path, sizeof(path) - 1);
#else
    isize = readlink("/proc/self/exe", path, sizeof(path) - 1);
#endif
    if (INIT_NUMBER >= isize
        || sizeof(path) <= (size_t)isize)
    {
        return "";
    }

    return dirnam(path) + PATH_SEPARATOR;
}
void timeofday(struct timeval *ptv)
{
#ifdef OS_WIN
    #define U64_LITERAL(n) n##ui64
    #define EPOCH_BIAS U64_LITERAL(116444736000000000)
    #define UNITS_PER_SEC U64_LITERAL(10000000)
    #define USEC_PER_SEC U64_LITERAL(1000000)
    #define UNITS_PER_USEC U64_LITERAL(10)
    union
    {
        FILETIME ft_ft;
        uint64_t ft_64;
    } ft;

    GetSystemTimeAsFileTime(&ft.ft_ft);
    ft.ft_64 -= EPOCH_BIAS;
    ptv->tv_sec = (long)(ft.ft_64 / UNITS_PER_SEC);
    ptv->tv_usec = (long)((ft.ft_64 / UNITS_PER_USEC) % USEC_PER_SEC);
#else
    (void)gettimeofday(ptv, NULL);
#endif
}
uint64_t nowmsec()
{
    struct timeval tv;
    timeofday(&tv);

    return (uint64_t)tv.tv_usec / 1000 + (uint64_t)tv.tv_sec * 1000;
}
uint64_t nowsec()
{
    struct timeval tv;
    timeofday(&tv);

    return (uint64_t)tv.tv_sec;
}
void nowtime(const char *pformat, char atime[TIME_LENS])
{
    struct timeval tv;
    timeofday(&tv);
    time_t t = tv.tv_sec;
    ZERO(atime, TIME_LENS);
    strftime(atime, TIME_LENS - 1, pformat, localtime(&t));
}
void nowmtime(const char *pformat, char atime[TIME_LENS])
{
    struct timeval tv;
    timeofday(&tv);
    time_t t = tv.tv_sec;
    ZERO(atime, TIME_LENS);
    strftime(atime, TIME_LENS - 1, pformat, localtime(&t));
    size_t uilen = strlen(atime);
    SNPRINTF(atime + uilen, TIME_LENS - uilen - 1, " %d", tv.tv_usec / 1000);
}
int32_t socknread(const SOCKET &fd)
{
#ifdef OS_WIN
    unsigned long ulread = INIT_NUMBER;
    if (ioctlsocket(fd, FIONREAD, &ulread) < 0)
    {
        return ERR_FAILED;
    }

    return (int32_t)ulread;
#else
    int32_t iread = INIT_NUMBER;
    if (ioctl(fd, FIONREAD, &iread) < 0)
    {
        return ERR_FAILED;
    }

    return iread;
#endif
}
int32_t sockrecv(const SOCKET &fd, class cbuffer *pbuf)
{
    return 0;
}
SOCKET socklsn(const char *ip, const uint16_t &port, const int32_t &backlog)
{
    if (INIT_NUMBER == backlog)
    {
        return INVALID_SOCK;
    }

    cnetaddr addr;
    if (!addr.setaddr(ip, port))
    {
        return INVALID_SOCK;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCK == fd)
    {
        return INVALID_SOCK;
    }
    if (ERR_OK != bind(fd, addr.getaddr(), (int32_t)addr.getsize()))
    {
        SAFE_CLOSESOCK(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(fd, (-1 == backlog) ? 128 : backlog))
    {
        SAFE_CLOSESOCK(fd);
        return INVALID_SOCK;
    }

    return fd;
}
SOCKET sockcnt(const char *ip, const uint16_t &port)
{
    cnetaddr addr;
    if (!addr.setaddr(ip, port))
    {
        return INVALID_SOCK;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCK == fd)
    {
        return INVALID_SOCK;
    }
    if (ERR_OK != connect(fd, addr.getaddr(), (int32_t)addr.getsize()))
    {
        SAFE_CLOSESOCK(fd);
        return INVALID_SOCK;
    }

    return fd;
}
void sockopts(SOCKET &fd)
{
    int nodelay =1;
    int keepalive = 1;

    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, (int)sizeof(nodelay));
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&keepalive, (int)sizeof(keepalive));

#ifdef OS_WIN
    unsigned long nonblocking = 1;
    (void)ioctlsocket(fd, FIONBIO, &nonblocking);
#else
    int flags = fcntl(fd, F_GETFL, NULL);
    if (ERR_FAILED == flags)
    {
        PRINTF("fcntl(%d, F_GETFL) error.", fd);
        return;
    }
    if (!(flags & O_NONBLOCK)) 
    {
        if (ERR_FAILED == fcntl(fd, F_SETFL, flags | O_NONBLOCK))
        {
            PRINTF("fcntl(%d, F_SETFL)", fd);
            return;
        }
    }
#endif
}
bool sockpair(SOCKET acSock[2])
{
    SOCKET fdlsn = socklsn("127.0.0.1", 0, 1);
    if (INVALID_SOCK == fdlsn)
    {
        return false;
    }

    cnetaddr addr;
    if (!addr.setlocaddr(fdlsn))
    {
        SAFE_CLOSESOCK(fdlsn);
        return false;
    }
    SOCKET fdcn = sockcnt(addr.getip().c_str(), addr.getport());
    if (INVALID_SOCK == fdcn)
    {
        SAFE_CLOSESOCK(fdlsn);
        return false;
    }

    struct sockaddr_in listen_addr;
    int32_t isize = sizeof(listen_addr);
    SOCKET fdacp = accept(fdlsn, (struct sockaddr *) &listen_addr, &isize);
    if (INVALID_SOCK == fdacp)
    {
        SAFE_CLOSESOCK(fdlsn);
        SAFE_CLOSESOCK(fdcn);
        return false;
    }
    SAFE_CLOSESOCK(fdlsn);
    if (!addr.setlocaddr(fdcn))
    {
        SAFE_CLOSESOCK(fdacp);
        SAFE_CLOSESOCK(fdcn);
        return false;
    }
    struct sockaddr_in *connect_addr = (sockaddr_in*)addr.getaddr();
    if (listen_addr.sin_family != connect_addr->sin_family
        || listen_addr.sin_addr.s_addr != connect_addr->sin_addr.s_addr
        || listen_addr.sin_port != connect_addr->sin_port)
    {
        SAFE_CLOSESOCK(fdacp);
        SAFE_CLOSESOCK(fdcn);
        return false;
    }

    sockopts(fdacp);
    sockopts(fdcn);
    acSock[0] = fdacp;
    acSock[1] = fdcn;

    return true;
}
const uint16_t crc16_tab[256] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};
uint16_t crc16(const char *pval, const size_t &ilen)
{
    uint16_t uscrc = INIT_NUMBER;
    const uint8_t *ptmp = (const uint8_t *)pval;
    for (size_t i = 0; i < ilen; ++i)
    {
        uscrc = (uscrc >> 8) ^ crc16_tab[(uscrc ^ *ptmp++) & 0xFF];
    }

    return uscrc;
}
static const uint32_t crc32_tab[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba,
    0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
    0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
    0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940,
    0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116,
    0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a,
    0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818,
    0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c,
    0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
    0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086,
    0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4,
    0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
    0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe,
    0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252,
    0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60,
    0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04,
    0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
    0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e,
    0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
    0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0,
    0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6,
    0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};
uint32_t crc32(const char *pval, const size_t &ilen)
{
    uint32_t uicrc = ~0U;
    const uint8_t *ptmp = (const uint8_t *)pval;
    for (size_t i = 0; i < ilen; ++i)
    {
        uicrc = crc32_tab[(uicrc ^ *ptmp++) & 0xFF] ^ (uicrc >> 8);
    }

    return uicrc ^ ~0U;
}
uint64_t siphash64(const uint8_t *pin, const size_t &inlen,
    const uint64_t &seed0, const uint64_t &seed1)
{
#define U8TO64_LE(p) \
    {  (((uint64_t)((p)[0])) | ((uint64_t)((p)[1]) << 8) | \
        ((uint64_t)((p)[2]) << 16) | ((uint64_t)((p)[3]) << 24) | \
        ((uint64_t)((p)[4]) << 32) | ((uint64_t)((p)[5]) << 40) | \
        ((uint64_t)((p)[6]) << 48) | ((uint64_t)((p)[7]) << 56)) }
#define U64TO8_LE(p, v) \
    { U32TO8_LE((p), (uint32_t)((v))); \
      U32TO8_LE((p) + 4, (uint32_t)((v) >> 32)); }
#define U32TO8_LE(p, v) \
    { (p)[0] = (uint8_t)((v)); \
      (p)[1] = (uint8_t)((v) >> 8); \
      (p)[2] = (uint8_t)((v) >> 16); \
      (p)[3] = (uint8_t)((v) >> 24); }
#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND \
    { v0 += v1; v1 = ROTL(v1, 13); \
      v1 ^= v0; v0 = ROTL(v0, 32); \
      v2 += v3; v3 = ROTL(v3, 16); \
      v3 ^= v2; \
      v0 += v3; v3 = ROTL(v3, 21); \
      v3 ^= v0; \
      v2 += v1; v1 = ROTL(v1, 17); \
      v1 ^= v2; v2 = ROTL(v2, 32); }
    uint64_t k0 = U8TO64_LE((uint8_t*)&seed0);
    uint64_t k1 = U8TO64_LE((uint8_t*)&seed1);
    uint64_t v3 = UINT64_C(0x7465646279746573) ^ k1;
    uint64_t v2 = UINT64_C(0x6c7967656e657261) ^ k0;
    uint64_t v1 = UINT64_C(0x646f72616e646f6d) ^ k1;
    uint64_t v0 = UINT64_C(0x736f6d6570736575) ^ k0;
    const uint8_t *end = pin + inlen - (inlen % sizeof(uint64_t));
    for (; pin != end; pin += 8) {
        uint64_t m = U8TO64_LE(pin);
        v3 ^= m;
        SIPROUND; SIPROUND;
        v0 ^= m;
    }
    const int32_t left = inlen & 7;
    uint64_t b = ((uint64_t)inlen) << 56;
    switch (left) {
    case 7: b |= ((uint64_t)pin[6]) << 48;
    case 6: b |= ((uint64_t)pin[5]) << 40;
    case 5: b |= ((uint64_t)pin[4]) << 32;
    case 4: b |= ((uint64_t)pin[3]) << 24;
    case 3: b |= ((uint64_t)pin[2]) << 16;
    case 2: b |= ((uint64_t)pin[1]) << 8;
    case 1: b |= ((uint64_t)pin[0]); break;
    case 0: break;
    }
    v3 ^= b;
    SIPROUND; SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;
    b = v0 ^ v1 ^ v2 ^ v3;
    uint64_t out = 0;
    U64TO8_LE((uint8_t*)&out, b);

    return out;
}
uint64_t murmurhash3(const void *key, const size_t &len, const uint32_t &seed)
{
#define	ROTL32(x, r) ((x << r) | (x >> (32 - r)))
#define FMIX32(h) h^=h>>16; h*=0x85ebca6b; h^=h>>13; h*=0xc2b2ae35; h^=h>>16;
    const uint8_t * data = (const uint8_t*)key;
    const int32_t nblocks = (int32_t)len / 16;
    uint32_t h1 = seed;
    uint32_t h2 = seed;
    uint32_t h3 = seed;
    uint32_t h4 = seed;
    uint32_t c1 = 0x239b961b;
    uint32_t c2 = 0xab0e9789;
    uint32_t c3 = 0x38b34ae5;
    uint32_t c4 = 0xa1e38b93;
    const uint32_t * blocks = (const uint32_t *)(data + nblocks * 16);
    for (int32_t i = -nblocks; i; i++) {
        uint32_t k1 = blocks[i * 4 + 0];
        uint32_t k2 = blocks[i * 4 + 1];
        uint32_t k3 = blocks[i * 4 + 2];
        uint32_t k4 = blocks[i * 4 + 3];
        k1 *= c1; k1 = ROTL32(k1, 15); k1 *= c2; h1 ^= k1;
        h1 = ROTL32(h1, 19); h1 += h2; h1 = h1 * 5 + 0x561ccd1b;
        k2 *= c2; k2 = ROTL32(k2, 16); k2 *= c3; h2 ^= k2;
        h2 = ROTL32(h2, 17); h2 += h3; h2 = h2 * 5 + 0x0bcaa747;
        k3 *= c3; k3 = ROTL32(k3, 17); k3 *= c4; h3 ^= k3;
        h3 = ROTL32(h3, 15); h3 += h4; h3 = h3 * 5 + 0x96cd1c35;
        k4 *= c4; k4 = ROTL32(k4, 18); k4 *= c1; h4 ^= k4;
        h4 = ROTL32(h4, 13); h4 += h1; h4 = h4 * 5 + 0x32ac3b17;
    }
    const uint8_t * tail = (const uint8_t*)(data + nblocks * 16);
    uint32_t k1 = 0;
    uint32_t k2 = 0;
    uint32_t k3 = 0;
    uint32_t k4 = 0;
    switch (len & 15) {
    case 15: k4 ^= tail[14] << 16;
    case 14: k4 ^= tail[13] << 8;
    case 13: k4 ^= tail[12] << 0;
        k4 *= c4; k4 = ROTL32(k4, 18); k4 *= c1; h4 ^= k4;
    case 12: k3 ^= tail[11] << 24;
    case 11: k3 ^= tail[10] << 16;
    case 10: k3 ^= tail[9] << 8;
    case  9: k3 ^= tail[8] << 0;
        k3 *= c3; k3 = ROTL32(k3, 17); k3 *= c4; h3 ^= k3;
    case  8: k2 ^= tail[7] << 24;
    case  7: k2 ^= tail[6] << 16;
    case  6: k2 ^= tail[5] << 8;
    case  5: k2 ^= tail[4] << 0;
        k2 *= c2; k2 = ROTL32(k2, 16); k2 *= c3; h2 ^= k2;
    case  4: k1 ^= tail[3] << 24;
    case  3: k1 ^= tail[2] << 16;
    case  2: k1 ^= tail[1] << 8;
    case  1: k1 ^= tail[0] << 0;
        k1 *= c1; k1 = ROTL32(k1, 15); k1 *= c2; h1 ^= k1;
    };
    h1 ^= len; h2 ^= len; h3 ^= len; h4 ^= len;
    h1 += h2; h1 += h3; h1 += h4;
    h2 += h1; h3 += h1; h4 += h1;
    FMIX32(h1); FMIX32(h2); FMIX32(h3); FMIX32(h4);
    h1 += h2; h1 += h3; h1 += h4;
    h2 += h1; h3 += h1; h4 += h1;

    char out[16] = { 0 };
    ((uint32_t*)out)[0] = h1;
    ((uint32_t*)out)[1] = h2;
    ((uint32_t*)out)[2] = h3;
    ((uint32_t*)out)[3] = h4;

    return *(uint64_t*)out;
}
std::string formatv(const char *pformat, va_list args)
{
    size_t uisize = ONEK;
    char *pbuff = new(std::nothrow) char[uisize];
    ASSERTAB(NULL != pbuff, ERRSTR_MEMORY);
    int32_t inum = INIT_NUMBER;

    while (true)
    {        ZERO(pbuff, uisize);
        inum = vsnprintf(pbuff, uisize, pformat, args);
        if ((inum > -1)
            && (inum < (int32_t)uisize))
        {
            std::string strret(pbuff);
            SAFE_DELARR(pbuff);

            return strret;
        }
        //分配更大空间
        uisize = (inum > -1) ? (inum + 1) : uisize * 2;
        SAFE_DELARR(pbuff);
        pbuff = new(std::nothrow) char[uisize];
        ASSERTAB(NULL != pbuff, ERRSTR_MEMORY);
    }
}
std::string formatstr(const char *pformat, ...)
{
    va_list va;
    va_start(va, pformat);
    std::string strret = formatv(pformat, va);
    va_end(va);

    return strret;
}
const char *ptrim = " \r\n\t\v";
std::string triml(const std::string &str)
{
    size_t startpos = str.find_first_not_of(ptrim);
    return (startpos == std::string::npos) ? "" : str.substr(startpos);
}
std::string trimr(const std::string &str)
{
    size_t endpos = str.find_last_not_of(ptrim);
    return (endpos == std::string::npos) ? "" : str.substr(0, endpos + 1);
}
std::string trim(const std::string &str)
{
    return trimr(triml(str));
}
void addtoken(std::vector<std::string> &tokens, const std::string &strtmp, const bool &empty)
{
    if (empty)
    {
        tokens.push_back(strtmp);
        return;
    }
    if (!strtmp.empty())
    {
        tokens.push_back(strtmp);
    }
}
std::vector<std::string> split(const std::string &str, const char *pflag, const bool empty)
{
    std::vector<std::string> tokens;
    if (str.empty())
    {
        return tokens;
    }
    size_t ilens = strlen(pflag);
    if (INIT_NUMBER == ilens)
    {
        addtoken(tokens, str, empty);
        return tokens;
    }

    std::string::size_type start = INIT_NUMBER;
    std::string::size_type pos = str.find(pflag, start);
    if (std::string::npos == pos)
    {
        addtoken(tokens, str, empty);
        return tokens;
    }

    std::string strtmp;    
    while (std::string::npos != pos)
    {
        strtmp = str.substr(start, pos - start);
        addtoken(tokens, strtmp, empty);

        start = pos + ilens;
        pos = str.find(pflag, start);
        if (std::string::npos == pos)
        {
            strtmp = str.substr(start);
            addtoken(tokens, strtmp, empty);
        }
    }

    return tokens;
}

SREY_NS_END
