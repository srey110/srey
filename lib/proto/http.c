#include "http.h"
#include "loger.h"

enum {
    INIT = 0,
    CONTENT,
    CHUNKED
};
#define MAX_HEADLENS 4096
#define FLAG_LINE "\r\n"
#define FLAG_END "\r\n\r\n"
#define FLAG_CONTENT "content-length"
#define FLAG_CHUNKED "transfer-encoding"
#define CHUNKED_KEY "chunked"

static inline void *_http_parse(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    /*size_t total = buffer_size(buf);
    size_t lens = strlen(FLAG_END);
    int32_t headpos = buffer_search(buf, 0, 0, 0, FLAG_END, lens);
    if (ERR_FAILED == headpos) {
        if (total >= MAX_HEADLENS) {
            *closefd = 1;
            LOG_WARN("%s", "http head too long.");
        }
        return NULL;
    }
    ud->headlens = headpos + lens;

    lens = strlen(FLAG_CONTENT);
    int32_t keypos = buffer_search(buf, 1, 0, headpos, FLAG_CONTENT, lens);
    if (ERR_FAILED != keypos) {
        keypos = buffer_search(buf, 0, keypos + lens, headpos, ":", 1);
        if (ERR_FAILED == keypos) {
            *closefd = 1;
            LOG_WARN("parse %s failed.", FLAG_CONTENT);
            return NULL;
        }
        int32_t keyend = buffer_search(buf, 0, keypos + 1, ud->headlens, FLAG_LINE, strlen(FLAG_LINE));
        if (ERR_FAILED == keyend) {
            *closefd = 1;
            LOG_WARN("parse %s failed.", FLAG_CONTENT);
            return NULL;
        }
        char buflens[32] = {0};
        lens = keyend - keypos - 1;
        if (lens != buffer_copyout(buf, keypos + 1, buflens, lens)) {
            *closefd = 1;
            LOG_WARN("copye %s failed.", FLAG_CONTENT);
            return NULL;
        }
        ud->lens = atoi(buflens);
        lens = ud->lens + ud->headlens;
        if (total >= lens) {
            void *data;
            *size = lens;
            MALLOC(data, lens);
            ASSERTAB(lens == buffer_remove(buf, data, lens), "copy buffer error.")
            return data;
        } else {
            ud->status = CONTENT;
            return NULL;
        }
    } else {
        lens = strlen(FLAG_CHUNKED);
        keypos = buffer_search(buf, 1, 0, headpos, FLAG_CHUNKED, lens);
        if (ERR_FAILED != keypos) {
            int32_t keyend = buffer_search(buf, 1, keypos + lens, ud->headlens, FLAG_LINE, strlen(FLAG_LINE));
            if (ERR_FAILED == keypos) {
                *closefd = 1;
                LOG_WARN("parse %s failed.", FLAG_CHUNKED);
                return NULL;
            }
            if (ERR_FAILED != buffer_search(buf, 1, keypos + lens, keyend, CHUNKED_KEY, strlen(CHUNKED_KEY))) {
                ud->status = CHUNKED;
                void *data;
                *size = ud->headlens;
                MALLOC(data, ud->headlens);
                ASSERTAB(ud->headlens == buffer_remove(buf, data, ud->headlens), "copy buffer error.")
                return data;
            }
        }
    }
    void *data;
    *size = ud->headlens;
    MALLOC(data, ud->headlens);
    ASSERTAB(ud->headlens == buffer_remove(buf, data, ud->headlens), "copy buffer error.")
    return data;*/
    return NULL;
}
static inline void *_http_content(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    /*size_t total = buffer_size(buf);
    size_t lens = ud->headlens + ud->lens;
    if (total >= lens) {
        void *data;
        *size = lens;
        MALLOC(data, lens);
        ASSERTAB(lens == buffer_remove(buf, data, lens), "copy buffer error.")
        ud->status = INIT;
        return data;
    }*/
    return NULL;
}
static inline void *_http_chunked(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    /*size_t total = buffer_size(buf);
    size_t lens = strlen(FLAG_LINE);
    int32_t keypos = buffer_search(buf, 0, 0, 0, FLAG_LINE, lens);
    if (ERR_FAILED != keypos) {
        char buflens[32] = { 0 };
        if (keypos != buffer_copyout(buf, 0, buflens, keypos)) {
            *closefd = 1;
            return NULL;
        }
        size_t cklens = atoi(buflens);
        if (cklens > 0) {
            lens = cklens + keypos + lens + lens;
            if (total >= lens) {
                void *data;
                *size = lens;
                MALLOC(data, lens);
                ASSERTAB(lens == buffer_remove(buf, data, lens), "copy buffer error.");
                return data;
            }
        } else {
            lens = keypos + strlen(FLAG_END);
            if (total >= lens) {
                ud->status = INIT;
                void *data;
                *size = lens;
                MALLOC(data, lens);
                ASSERTAB(lens == buffer_remove(buf, data, lens), "copy buffer error.");
                return data;
            }
        }
    }*/
    return NULL;
}
void *http_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    /*void *data = NULL;
    switch (ud->status) {
    case INIT:
        data = _http_parse(buf, size, ud, closefd);
        break;
    case CONTENT:
        data = _http_content(buf, size, ud, closefd);
        break;
    case CHUNKED:
        data = _http_chunked(buf, size, ud, closefd);
        break;
    default:
        break;
    }
    return data;*/
    return NULL;
}
void http_pkfree(void *data) {
    
}
void http_exfree(void *extra) {

}
