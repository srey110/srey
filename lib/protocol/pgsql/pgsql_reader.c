#include "protocol/pgsql/pgsql_reader.h"
#include "protocol/pgsql/pgsql_parse.h"

pgsql_reader_ctx *pgsql_reader_init(pgpack_ctx *pgpack, pgpack_format format) {
    if (NULL == pgpack->pack
        || PGPACK_OK != pgpack->type) {
        return NULL;
    }
    pgsql_reader_ctx *reader = pgpack->pack;
    reader->format = format;
    // 将 pack 的所有权从 pgpack 转移到调用方，避免 _pgpack_free 时二次释放 reader
    pgpack->pack = NULL;
    pgpack->_free_pgpack = NULL;
    return reader;
}
void pgsql_reader_free(pgsql_reader_ctx *reader) {
    _pgpack_reader_free(reader); // 释放行数组与字段数组
    FREE(reader);
}
size_t pgsql_reader_size(pgsql_reader_ctx *reader) {
    return array_size(&reader->arr_rows);
}
void pgsql_reader_seek(pgsql_reader_ctx *reader, size_t pos) {
    if (pos >= array_size(&reader->arr_rows)) {
        return; // 目标位置超出范围，不移动游标
    }
    reader->index = (int32_t)pos;
}
int32_t pgsql_reader_eof(pgsql_reader_ctx *reader) {
    return (reader->index >= (int32_t)array_size(&reader->arr_rows)) ? 1 : 0;
}
void pgsql_reader_next(pgsql_reader_ctx *reader) {
    if (reader->index < (int32_t)array_size(&reader->arr_rows)) {
        reader->index++;
    }
}
pgpack_row *pgsql_reader_index(pgsql_reader_ctx *reader, int16_t index, pgpack_field **field) {
    if (reader->index >= (int32_t)array_size(&reader->arr_rows)
        || (index < 0 || index >= reader->field_count)) {
        return NULL;
    }
    pgpack_row *row = *(pgpack_row **)array_at(&reader->arr_rows, (uint32_t)reader->index);
    if (NULL != field
        && NULL != reader->fields) {
        *field = &reader->fields[index]; // 同时输出字段描述指针
    }
    return &row[index];
}
// 按字段名查找列索引，未找到返回 ERR_FAILED
static int32_t _pgsql_reader_index(pgsql_reader_ctx *reader, const char *name) {
    for (int32_t i = 0; i < reader->field_count; i++) {
        if (0 == strcmp(reader->fields[i].name, name)) {
            return i;
        }
    }
    return ERR_FAILED;
}
pgpack_row *pgsql_reader_name(pgsql_reader_ctx *reader, const char *name, pgpack_field **field) {
    if (NULL == reader->fields) {
        return NULL; // 无字段描述（未收到 RowDescription 消息）
    }
    int32_t index = _pgsql_reader_index(reader, name);
    if (ERR_FAILED == index) {
        return NULL;
    }
    return pgsql_reader_index(reader, index, field);
}
int32_t pgsql_reader_bool(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (BOOLOID != field->type_oid) { // 字段类型不是 bool
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (-1 == row->lens) { // NULL 值
        SET_PTR(err, 1);
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        // 文本格式：识别 t/true/y/yes/on/1 为真
        static const char *_pgsql_true[] = { "t", "true", "y", "yes", "on", "1" };
        int32_t n = (int32_t)ARRAY_SIZE(_pgsql_true);
        for (int32_t i = 0; i < n; i++) {
            if ((int32_t)strlen(_pgsql_true[i]) == row->lens
                && 0 == _memicmp(row->val, _pgsql_true[i], row->lens)) {
                return 1;
            }
        }
        return 0;
    }
    // 二进制格式：直接取第一个字节
    if (1 != row->lens) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    return row->val[0];
}
int64_t pgsql_reader_integer(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (INT2OID != field->type_oid
        && INT4OID != field->type_oid
        && INT8OID != field->type_oid) { // 字段类型不是整数类型
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (-1 == row->lens) { // NULL 值
        SET_PTR(err, 1);
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        // 文本格式：将字符串转为 int64
        char tmp[64];
        if (row->lens >= (int32_t)sizeof(tmp)) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0;
        }
        memcpy(tmp, row->val, row->lens);
        tmp[row->lens] = '\0';
        char *end;
        int64_t val = strtoll(tmp, &end, 10);
        if ((int32_t)(end - tmp) != row->lens) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0;
        }
        return val;
    }
    // 二进制格式：大端序整数解包
    int32_t expect = (INT2OID == field->type_oid) ? 2 : ((INT4OID == field->type_oid) ? 4 : 8);
    if (expect != row->lens) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    return unpack_integer(row->val, row->lens, 0, 1);
}
double pgsql_reader_double(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (FLOAT4OID != field->type_oid
        && FLOAT8OID != field->type_oid) { // 字段类型不是浮点类型
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (-1 == row->lens) { // NULL 值
        SET_PTR(err, 1);
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        // 文本格式：将字符串转为 double
        char tmp[128];
        if (row->lens >= (int32_t)sizeof(tmp)) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0.0;
        }
        memcpy(tmp, row->val, row->lens);
        tmp[row->lens] = '\0';
        char *end;
        double val = strtod(tmp, &end);
        if ((int32_t)(end - tmp) != row->lens) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0.0;
        }
        return val;
    }
    // 二进制格式：按长度区分 float4 / float8
    if (sizeof(double) == row->lens) {
        return unpack_double(row->val, 0);
    } else if (sizeof(float) == row->lens) {
        return unpack_float(row->val, 0);
    }
    SET_PTR(err, ERR_FAILED);
    return 0.0;
}
int32_t pgsql_reader_isnull(pgsql_reader_ctx *reader, const char *name) {
    pgpack_row *row = pgsql_reader_name(reader, name, NULL);
    if (NULL == row) {
        return 0; // 字段不存在，非 NULL
    }
    return (-1 == row->lens) ? 1 : 0;
}
const char *pgsql_reader_text(pgsql_reader_ctx *reader, const char *name, int32_t *lens, int32_t *err) {
    SET_PTR(lens, 0);
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    if (TEXTOID != field->type_oid
        && VARCHAROID != field->type_oid
        && BPCHAROID != field->type_oid
        && NAMEOID != field->type_oid
        && UNKNOWNOID != field->type_oid) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    if (-1 == row->lens) { // NULL 值
        SET_PTR(err, 1);
        return NULL;
    }
    // 文本/二进制格式均为 UTF-8 字节流，直接返回指针，不含 '\0' 结尾
    SET_PTR(lens, row->lens);
    return row->val;
}
const char *pgsql_reader_bytea(pgsql_reader_ctx *reader, const char *name, int32_t *lens, int32_t *err) {
    SET_PTR(lens, 0);
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    if (BYTEAOID != field->type_oid) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    if (-1 == row->lens) { // NULL 值
        SET_PTR(err, 1);
        return NULL;
    }
    // 二进制格式：原始字节；文本格式：'\x' 前缀 + 十六进制字符串（调用方自行解码）
    SET_PTR(lens, row->lens);
    return row->val;
}
// JDN 计算（与 PostgreSQL date2j 逻辑一致）：返回相对 PG 纪元（2000-01-01）的天数
static int32_t _pgsql_date_to_days(int32_t y, int32_t m, int32_t d) {
    int32_t century, julian;
    if (m > 2) { 
        m++; y += 4800; 
    } else {
        m += 13; y += 4799;
    }
    century = y / 100;
    julian = y * 365 - 32167;
    julian += y / 4 - century + century / 4;
    julian += 7834 * m / 256 + d;
    return julian - 2451545;
}
// 将文本格式时间戳 "YYYY-MM-DD HH:MM:SS[.ffffff]" 解析为相对 PG 纪元的微秒数
static int64_t _pgsql_usec_from_text(const char *s, int32_t slen, int32_t *err) {
    char tmp[48];
    if (slen >= (int32_t)sizeof(tmp)) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    memcpy(tmp, s, slen);
    tmp[slen] = '\0';
    int32_t y, mo, d, h, mi, sec;
    if (6 != sscanf(tmp, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec)) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    // 手动解析小数秒（sscanf 解析整数会丢失位数信息），按 6 位对齐为微秒
    int32_t usec = 0;
    const char *dot = strchr(tmp, '.');
    if (NULL != dot) {
        dot++;
        int32_t mult = 100000;
        for (int32_t i = 0; i < 6 && dot[i] >= '0' && dot[i] <= '9'; i++, mult /= 10) {
            usec += (dot[i] - '0') * mult;
        }
    }
    int64_t days = _pgsql_date_to_days(y, mo, d);
    return days * 86400000000LL
         + (int64_t)h * 3600000000LL
         + (int64_t)mi * 60000000LL
         + (int64_t)sec * 1000000LL
         + usec;
}
// 将文本格式日期 "YYYY-MM-DD" 解析为相对 PG 纪元的天数
static int32_t _pgsql_days_from_text(const char *s, int32_t slen, int32_t *err) {
    char tmp[16];
    if (slen >= (int32_t)sizeof(tmp)) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    memcpy(tmp, s, slen);
    tmp[slen] = '\0';
    int32_t y, m, d;
    if (3 != sscanf(tmp, "%d-%d-%d", &y, &m, &d)) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    return _pgsql_date_to_days(y, m, d);
}
// 将十六进制字符转为整数值，无效字符返回 -1
static int32_t _pgsql_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
int64_t pgsql_reader_timestamp(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (TIMESTAMPOID != field->type_oid
        && TIMESTAMPTZOID != field->type_oid) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (-1 == row->lens) { // NULL 值
        SET_PTR(err, 1);
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        return _pgsql_usec_from_text(row->val, row->lens, err);
    }
    // 二进制格式：大端序 int64，相对 PG 纪元的微秒数
    if (8 != row->lens) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    return (int64_t)unpack_integer(row->val, row->lens, 0, 1);
}
int32_t pgsql_reader_date(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (DATEOID != field->type_oid) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (-1 == row->lens) { // NULL 值
        SET_PTR(err, 1);
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        return _pgsql_days_from_text(row->val, row->lens, err);
    }
    // 二进制格式：大端序 int32，相对 PG 纪元的天数
    if (4 != row->lens) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    return (int32_t)unpack_integer(row->val, row->lens, 0, 1);
}
// 将文本格式 UUID "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"（36 字符）解析为 16 字节
static int32_t _pgsql_uuid_from_text(const char *s, int32_t lens, char uuid[16]) {
    if (36 != lens) {
        return ERR_FAILED;
    }
    int32_t hi, lo;
    int32_t bi = 0;
    for (int32_t i = 0; i < 36; ) {
        if ('-' == s[i]) {
            i++;
            continue;
        }
        if (i + 1 >= 36 || bi >= 16) {
            return ERR_FAILED;
        }
        hi = _pgsql_hex_digit(s[i]);
        lo = _pgsql_hex_digit(s[i + 1]);
        if (hi < 0 || lo < 0) {
            return ERR_FAILED;
        }
        uuid[bi++] = (char)((hi << 4) | lo);
        i += 2;
    }
    return (16 == bi) ? ERR_OK : ERR_FAILED;
}
int32_t pgsql_reader_uuid(pgsql_reader_ctx *reader, const char *name, char uuid[16], int32_t *err) {
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return ERR_FAILED;
    }
    if (UUIDOID != field->type_oid) {
        SET_PTR(err, ERR_FAILED);
        return ERR_FAILED;
    }
    if (-1 == row->lens) { // NULL 值
        SET_PTR(err, 1);
        return ERR_FAILED;
    }
    if (FORMAT_BINARY == reader->format) {
        if (16 != row->lens) {
            SET_PTR(err, ERR_FAILED);
            return ERR_FAILED;
        }
        memcpy(uuid, row->val, 16);
        return ERR_OK;
    }
    // 文本格式："xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"（36 字符）
    if (ERR_OK != _pgsql_uuid_from_text(row->val, row->lens, uuid)) {
        SET_PTR(err, ERR_FAILED);
        return ERR_FAILED;
    }
    return ERR_OK;
}
