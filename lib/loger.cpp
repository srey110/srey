#include "loger.h"
#include "chan.h"
#include "thread.h"

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
    clogertask(cchan *chan, volatile ATOMIC_T *print) :
        m_chan(chan), m_print(print), m_file(NULL)
    {
        //´´½¨ÎÄ¼þ¼Ð
        m_path = getpath() + PATH_SEPARATOR + "logs";
        if (!fileexist(m_path.c_str()))
        {
            if (ERR_OK != MKDIR(m_path.c_str()))
            {
                PRINTF("mkdir(%s) error.", m_path.c_str());
                m_path.clear();
            }
        }
#ifdef OS_WIN
        //»ñÈ¡±ê×¼Êä³ö
        m_hout = GetStdHandle(STD_OUTPUT_HANDLE);
        if (INVALID_HANDLE_VALUE == m_hout
            || NULL == m_hout)
        {
            m_hout = NULL;
            PRINTF("%s", "GetStdHandle(STD_OUTPUT_HANDLE) error.");
            return;
        }
        if (!GetConsoleScreenBufferInfo(m_hout, &m_stcsbi))
        {
            PRINTF("%s", "GetConsoleScreenBufferInfo error.");
            return;
        }
#endif  
    };
    ~clogertask()
    {
        if (NULL != m_file)
        {
            fclose(m_file);
            m_file = NULL;
        }
    };
    bool getfile()
    {
        if (m_path.empty())
        {
            return false;
        }
        //ÅÐ¶ÏÈÕÆÚÊÇ·ñ¸ü¸Ä£¬¸ü¸ÄÔò¸ü»»logÎÄ¼þÃû
        nowtime("%Y%m%d", m_date);
        if (m_filename != m_date)
        {
            if (NULL != m_file)
            {
                fclose(m_file);
                m_file = NULL;
            }
            m_filename = m_date;
        }
        //´ò¿ªÎÄ¼þ
        if (NULL == m_file)
        {
            std::string strfilename = m_path + PATH_SEPARATOR + m_filename + ".log";
            m_file = fopen(strfilename.c_str(), "a");
            if (NULL == m_file)
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
        while (!isstop())
        {
            pinfo = NULL;
            if (!m_chan->recv(&pinfo))
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

private:
    void writelog(loginfo *pinfo)
    {
        const char *pn = "\n";
        (void)fwrite(pinfo->plog, 1, strlen(pinfo->plog), m_file);
        (void)fwrite(pn, 1, strlen(pn), m_file);
    };
    void printlog(loginfo *pinfo)
    {
        if (INIT_NUMBER == ATOMIC_GET(m_print))
        {
            return;
        }

#ifdef OS_WIN
        if (NULL == m_hout)
        {
            return;
        }

        switch (pinfo->lv)
        {
        case LOGLV_FATAL:
            SetConsoleTextAttribute(m_hout, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | BACKGROUND_RED );
            break;
        case LOGLV_ERROR:
            SetConsoleTextAttribute(m_hout, FOREGROUND_RED );
            break;
        case LOGLV_WARN:
            SetConsoleTextAttribute(m_hout, FOREGROUND_RED | FOREGROUND_GREEN);
            break;
        case LOGLV_INFO:
            SetConsoleTextAttribute(m_hout, FOREGROUND_GREEN);
            break;
        case LOGLV_DEBUG:
            SetConsoleTextAttribute(m_hout, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            break;
        default:
            break;
        }

        printf("%s\n", pinfo->plog);
        SetConsoleTextAttribute(m_hout, m_stcsbi.wAttributes);
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
    cchan *m_chan;
    volatile ATOMIC_T *m_print;
    FILE *m_file;
#ifdef OS_WIN
    HANDLE m_hout;
    CONSOLE_SCREEN_BUFFER_INFO m_stcsbi;
#endif
    char m_date[TIME_LENS];
    std::string m_path;
    std::string m_filename;
};

cloger::cloger()
{
    m_lv = LOGLV_DEBUG;
    m_print = 1;
    m_chan = new(std::nothrow) cchan(ONEK);
    ASSERTAB(NULL != m_chan, ERRSTR_MEMORY);
    m_thread = new(std::nothrow) cthread();
    ASSERTAB(NULL != m_thread, ERRSTR_MEMORY);
    m_task = new(std::nothrow) clogertask(m_chan, &m_print);
    ASSERTAB(NULL != m_task, ERRSTR_MEMORY);
    m_thread->creat(m_task);
}
cloger::~cloger()
{
    while (m_chan->size() > INIT_NUMBER);

    m_chan->close();
    m_task->stop();
    m_thread->join();

    SAFE_DEL(m_chan);
    SAFE_DEL(m_thread);
    SAFE_DEL(m_task);
}
void cloger::setlv(const LOG_LEVEL &emlv)
{
    ATOMIC_SET(&m_lv, emlv);
}
void cloger::setprint(const bool &bprint)
{
    bprint ? ATOMIC_SET(&m_print, 1) : ATOMIC_SET(&m_print, INIT_NUMBER);
}
void cloger::log(const LOG_LEVEL &emlv, const char *pformat, ...)
{
    if ((uint32_t)emlv > ATOMIC_GET(&m_lv))
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

    if (!m_chan->send((void*)pinfo))
    {
        PRINTF("write log error. %s", pinfo->plog);
        SAFE_DELARR(pinfo->plog);
        SAFE_DEL(pinfo);
    }
}
const char *cloger::_getlvstr(const LOG_LEVEL &emlv)
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
