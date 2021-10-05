#ifndef INCLUDE_H_
#define INCLUDE_H_

#include "os.h"
#include <string>
#include <list>
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <fcntl.h>
#include <wchar.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/timeb.h>

#ifdef OS_WIN
    #include <winsock2.h>
    #include <ws2ipdef.h>
    #include <ws2tcpip.h>
    #include <TlHelp32.h>
    #include <io.h>
    #include <tchar.h>
    #include <direct.h>
    #include <process.h>
    #include <ObjBase.h>
    #include <Windows.h>
    #include <MSTcpIP.h>
#else
    #include <unistd.h>
    #include <signal.h>
    #include <netinet/in.h>
    #include <errno.h>
    #include <dirent.h>
    #include <libgen.h>    
    #include <dlfcn.h>
    #include <locale.h>
    #include <syslog.h>
    #include <sys/statvfs.h>
    #include <sys/msg.h>
    #include <sys/sem.h>
    #include <sys/ipc.h>
    #include <sys/errno.h>
    #include <netinet/tcp.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <semaphore.h>
    #include <sys/socketvar.h>
    #include <sys/resource.h>
    #include <sys/mman.h>
    #include <sys/time.h>
    #include <sys/wait.h>
    #include <sys/ioctl.h>
    #include <net/if.h>
    #include <sys/syscall.h>
    #include <net/if_arp.h>
#ifdef OS_LINUX
    #include <linux/limits.h>
#endif
#endif // OS_WIN

#endif//INCLUDE_H_
