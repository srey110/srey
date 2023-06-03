#ifndef PROTOCB_H_
#define PROTOCB_H_

#include "event/evpub.h"

typedef void(*push_acptmsg)(SOCKET fd, ud_cxt *ud);
typedef void(*push_connmsg)(SOCKET fd, int32_t err, ud_cxt *ud);

#endif//PROTOCB_H_
