#include "tasks.h"
#if WITH_LUA
#include "ltasks/ltask.h"
#endif

int32_t task_startup() {
    int32_t rtn = ERR_OK;
#if WITH_LUA
    rtn = _ltask_startup();
#endif
    return rtn;
}
