#include "protocol/pgsql/pgsql_bind.h"

void pgsql_bind_init(pgsql_bind_ctx *bind, uint16_t nparam) {
    ZERO(bind, sizeof(pgsql_bind_ctx));
    bind->nparam = nparam;
    if (0 == bind->nparam) {
        return;
    }
    CALLOC(bind->values, 1, sizeof(buf_ctx) * bind->nparam);
    CALLOC(bind->format, 1, sizeof(pgpack_format) * bind->nparam);
}
void pgsql_bind_free(pgsql_bind_ctx *bind) {
    if (0 == bind->nparam) {
        return;
    }
    FREE(bind->values);
    FREE(bind->format);
}
void pgsql_bind_clear(pgsql_bind_ctx *bind) {
    if (0 == bind->nparam) {
        return;
    }
    ZERO(bind->values, sizeof(buf_ctx) * bind->nparam);
    ZERO(bind->format, sizeof(pgpack_format) * bind->nparam);
}
void pgsql_bind(pgsql_bind_ctx *bind, uint16_t index, char *val, size_t lens, pgpack_format format) {
    ASSERTAB(index >= 0 && index < bind->nparam, "out of range.");
    bind->values[index].data = val;
    bind->values[index].lens = lens;
    bind->format[index] = format;
}
