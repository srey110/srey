#ifndef EVENT_H_
#define EVENT_H_

#include "wot.h"
#include "timer.h"

typedef struct event_ctx
{
    struct sock_ctx sock;
    struct wot_ctx wotctc;
}event_ctx;

#endif//EVENT_H_
