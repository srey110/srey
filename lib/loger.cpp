#include "loger.h"
#include "chan.h"
#include "thread.h"
#include "utils.h"
#include "errcode.h"

SREY_NS_BEGIN

#ifndef OS_WIN
#define CLR_CLR         "\033[0m"       /* »Ö¸´ÑÕÉ« */
#define CLR_BLACK       "\033[30m"      /* ºÚÉ«×Ö */
#define CLR_RED         "\033[31m"      /* ºìÉ«×Ö */
#define CLR_GREEN       "\033[32m"      /* ÂÌÉ«×Ö */
#define CLR_YELLOW      "\033[33m"      /* »ÆÉ«×Ö */
#define CLR_BLUE        "\033[34m"      /* À¶É«×Ö */
#define CLR_PURPLE      "\033[35m"      /* ×ÏÉ«×Ö */
#define CLR_SKYBLUE     "\033[36m"      /* ÌìÀ¶×Ö */
#define CLR_WHITE       "\033[37m"      /* °×É«×Ö */

#define CLR_BLK_WHT     "\033[40;37m"   /* ºÚµ×°××Ö */
#define CLR_RED_WHT     "\033[41;37m"   /* ºìµ×°××Ö */
#define CLR_GREEN_WHT   "\033[42;37m"   /* ÂÌµ×°××Ö */
#define CLR_YELLOW_WHT  "\033[43;37m"   /* »Æµ×°××Ö */
#define CLR_BLUE_WHT    "\033[44;37m"   /* À¶µ×°××Ö */
#define CLR_PURPLE_WHT  "\033[45;37m"   /* ×Ïµ×°××Ö */
#define CLR_SKYBLUE_WHT "\033[46;37m"   /* ÌìÀ¶µ×°××Ö */
#define CLR_WHT_BLK     "\033[47;30m"   /* °×µ×ºÚ×Ö */
#endif

SINGLETON_INIT(cloger)
cloger g_loger;

struct loginfo
{
    LOG_LEVEL lv;
    char *plog;
};

class clogertask : public ctask
{
public:
    clogertask(cchan *chan, uint32_t *print) : 
        uiloop(INIT_NUMBER), pchan(chan), isprint(print), pfile(NULL)
    {
        //´´½¨ÎÄ¼þ¼Ð
        path = getpath() + PATH_SEPARATOR + "logs";
        if (!fileexist(path.c_str()))
        {
            if (ERR_OK != MKDIR(path.c_str()))
            {
                PRINTF("mkdir(%s) error.", path.c_str());
                path.clear();
            }
        }
#ifdef OS_WIN
        //»ñÈ¡±ê×¼Êä³ö
        hout = GetStdHandle(STD_OUTPUT_HANDLE);
        if (INVALID_HANDLE_VALUE == hout
            || NULL == hout)
        {
            hout = NULL;
            PRINTF("%s", "GetStdHandle(STD_OUTPUT_HANDLE) error.");
            return;
        }
        if (!GetConsoleScreenBufferInfo(hout, &stcsbi))
        {
            PRINTF("%s", "GetConsoleScreenBufferInfo error.");
            return;
        }
#endif  
    };
    ~clogertask()
    {
        if (NULL != pfile)
        {
            fclose(pfile);
            pfile = NULL;
        }
    };
    bool getfile()
    {
        if (path.empty())
        {
            return false;
        }
        //ÅÐ¶ÏÈÕÆÚÊÇ·ñ¸ü¸Ä£¬¸ü¸ÄÔò¸ü»»logÎÄ¼þÃû
        nowtime("%Y%m%d", date);
        if (filename != date)
        {
            if (NULL != pfile)
            {
                fclose(pfile);
                pfile = NULL;
            }
            filename = date;
        }
        //´ò¿ªÎÄ¼þ
        if (NULL == pfile)
        {
            std::string strfilename = path + PATH_SEPARATOR + filename + ".log";
            pfile = fopen(strfilename.c_str(), "a");
            if (NULL == pfile)
            {
                PRINTF("fopen(%s, a) error.", strfilename.c_str());
                return false;
            }
        }

        return true;
    };
    void run()
    {
        void *pinfo;
        while (INIT_NUMBER == ATOMIC_GET(&uiloop))
        {
            pinfo = NULL;
            if (!pchan->recv(&pinfo))
            {
                freeloginfo((loginfo *)pinfo);
                continue;
            }

            printlog((loginfo *)pinfo);
            if (!getfile())
            {
                freeloginfo((loginfo *)pinfo);
                continue;
            }

            writelog((loginfo *)pinfo);
            freeloginfo((loginfo *)pinfo);
        }
    };
    void stop()
    {
        ATOMIC_ADD(&uiloop, 1);
    };

private:
    void writelog(loginfo *pinfo)
    {
        const char *pn = "\n";
        (void)fwrite(pinfo->plog, 1, strlen(pinfo->plog), pfile);
        (void)fwrite(pn, 1, strlen(pn), pfile);
    };
    void printlog(loginfo *pinfo)
    {
        if (INIT_NUMBER == ATOMIC_GET(isprint))
        {
            return;
        }

#ifdef OS_WIN
        if (NULL == hout)
        {
            return;
        }

        switch (pinfo->lv)
        {
        case LOGLV_FATAL:
            SetConsoleTextAttribute(hout, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | BACKGROUND_RED );
            break;
        case LOGLV_ERROR:
            SetConsoleTextAttribute(hout, FOREGROUND_RED );
            break;
        case LOGLV_WARN:
            SetConsoleTextAttribute(hout, FOREGROUND_RED | FOREGROUND_GREEN);
            break;
        case LOGLV_INFO:
            SetConsoleTextAttribute(hout, FOREGROUND_GREEN);
            break;
        case LOGLV_DEBUG:
            SetConsoleTextAttribute(hout, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            break;
        default:
            break;
        }

        printf("%s\n", pinfo->plog);
        SetConsoleTextAttribute(hout, stcsbi.wAttributes);
#else
        switch (pinfo->lv)
        {
        case LOGLV_FATAL:
            printf("%s%s%s\n", CLR_RED_WHT, pinfo->plog, CLR_CLR);
            break;
        case LOGLV_ERROR:
            printf("%s%s%s\n", CLR_RED, pinfo->plog, CLR_CLR);
            break;
        case LOGLV_WARN:
            printf("%s%s%s\n", CLR_YELLOW, pinfo->plog, CLR_CLR);
            break;
        case LOGLV_INFO:
            printf("%s%s%s\n", CLR_GREEN, pinfo->plog, CLR_CLR);
            break;
        case LOGLV_DEBUG:
            printf("%s%s%s\n", CLR_WHITE, pinfo->plog, CLR_CLR);
            break;
        }
#endif
    };
    void freeloginfo(loginfo *pinfo)
    {
        if (NULL != pinfo)
        {
            SAFE_DELARR(pinfo->plog);
            SAFE_DEL(pinfo);
        }
    };

private:
    uint32_t uiloop;
    cchan *pchan;
    uint32_t *isprint;
    FILE *pfile;
#ifdef OS_WIN
    HANDLE hout;
    CONSOLE_SCREEN_BUFFER_INFO stcsbi;
#endif
    char date[TIME_LENS];
    std::string path;
    std::string filename;
};

cloger::cloger()
{
    uilv = LOGLV_DEBUG;
    uiprint = 1;
    pchan = new(std::nothrow) cchan(ONEK);
    ASSERTAB(NULL != pchan, ERRSTR_MEMORY);
    pthread = new(std::nothrow) cthread();
    ASSERTAB(NULL != pthread, ERRSTR_MEMORY);
    ptask = new(std::nothrow) clogertask(pchan, &uiprint);
    ASSERTAB(NULL != ptask, ERRSTR_MEMORY);
    pthread->creat(ptask);
}
cloger::~cloger()
{
    while (pchan->size() > INIT_NUMBER);

    pchan->close();
    ptask->stop();
    pthread->join();

    SAFE_DEL(pchan);
    SAFE_DEL(pthread);
    SAFE_DEL(ptask);
}
void cloger::setlv(const LOG_LEVEL &emlv)
{
    ATOMIC_SET(&uilv, emlv);
}
void cloger::setprint(const bool &bprint)
{
    bprint ? ATOMIC_SET(&uiprint, 1) : ATOMIC_SET(&uiprint, INIT_NUMBER);
}
void cloger::log(const LOG_LEVEL &emlv, const char *pformat, ...)
{
    if ((uint32_t)emlv > ATOMIC_GET(&uilv))
    {
        return;
    }
    loginfo *pinfo = new(std::nothrow) loginfo();
    if (NULL == pinfo)
    {
        PRINTF("%s", ERRSTR_MEMORY);
        return;
    }

    pinfo->lv = emlv;

    va_list va;
    va_start(va, pformat);
    pinfo->plog = formatv(pformat, va);
    va_end(va);

    if (!pchan->send((void*)pinfo))
    {
        PRINTF("write log error. %s", pinfo->plog);
        SAFE_DELARR(pinfo->plog);
        SAFE_DEL(pinfo);
    }
}
const char *cloger::getlvstr(const LOG_LEVEL &emlv)
{
    switch (emlv)
    {
    case LOGLV_FATAL:
        return "FATAL";
    case LOGLV_ERROR:
        return "ERROR";
    case LOGLV_WARN:
        return " WARN";    
    case LOGLV_INFO:
        return " INFO";
    case LOGLV_DEBUG:
        return "DEBUG";
    default:
        break;
    }

    return "UNKNOWN";
}

SREY_NS_END
