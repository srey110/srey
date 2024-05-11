#include "srey/ssls.h"

#if WITH_SSL
static certs_ctx *_ssl_get(scheduler_ctx *scheduler, name_t name) {
    certs_ctx *cert;
    uint32_t n = arr_certs_size(&scheduler->arrcerts);
    for (uint32_t i = 0; i < n; i++) {
        cert = arr_certs_at(&scheduler->arrcerts, i);
        if (name == cert->name) {
            return cert;
        }
    }
    return NULL;
}
int32_t srey_ssl_register(scheduler_ctx *scheduler, name_t name, struct evssl_ctx *evssl) {
    if (NULL == evssl) {
        LOG_WARN("%s", ERRSTR_NULLP);
        return ERR_FAILED;
    }
    certs_ctx cert;
    cert.name = name;
    cert.ssl = evssl;
    int32_t rtn;
    rwlock_wrlock(&scheduler->lckcerts);
    if (NULL != _ssl_get(scheduler, name)) {
        LOG_ERROR("ssl name %d repeat.", name);
        rtn = ERR_FAILED;
    } else {
        arr_certs_push_back(&scheduler->arrcerts, &cert);
        rtn = ERR_OK;
    }
    rwlock_unlock(&scheduler->lckcerts);
    return rtn;
}
struct evssl_ctx *srey_ssl_qury(scheduler_ctx *scheduler, name_t name) {
    certs_ctx *cert;
    rwlock_rdlock(&scheduler->lckcerts);
    cert = _ssl_get(scheduler, name);
    rwlock_unlock(&scheduler->lckcerts);
    return NULL == cert ? NULL : cert->ssl;
}
#endif
