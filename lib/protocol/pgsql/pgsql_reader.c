#include "protocol/pgsql/pgsql_reader.h"
#include "protocol/pgsql/pgsql_parse.h"
#include "protocol/pgsql/pgsql.h"// pgsql_result_count
#include "utils/strptime.h"
#include "utils/uuid.h"

// 文本 bool 多字节真值表的一项：字面量与它的长度
typedef struct pgsql_bool_word {
    const char *s;
    int32_t n;
}pgsql_bool_word;

// 取走第 idx 个结果的 reader; 下标须已由调用方确认在范围内。所有权转移给调用方,
// 槽位置 NULL 以免 _pgpack_free 二次释放
static inline pgsql_reader_ctx *_pgsql_reader_take(pgpack_ctx *pgpack, uint32_t idx, pgpack_format format) {
    pgsql_result *res = pgres_arr_at(&pgpack->results, (int32_t)idx);
    if (NULL == res->reader) {
        return NULL; // 该语句无结果集（INSERT/UPDATE 无 RETURNING）
    }
    pgsql_reader_ctx *reader = res->reader;
    reader->format = format;
    res->reader = NULL;
    return reader;
}
pgsql_reader_ctx *pgsql_reader_iter(pgpack_ctx *pgpack, pgpack_format format) {
    // 从上次停下的位置接着扫：槽位一旦取空就再不会变回非空，所以游标只前进不回头，
    // 遍历整个响应是 O(n) 而不是每次都从 0 重扫。BEGIN / SET 这类无结果集的槽位一并跳过
    uint32_t total = pgsql_result_count(pgpack);// 类型不符时返 0, 循环不进
    pgsql_reader_ctx *reader;
    while (pgpack->iter_cursor < total) {
        reader = _pgsql_reader_take(pgpack, pgpack->iter_cursor, format);
        pgpack->iter_cursor++;
        if (NULL != reader) {
            return reader;
        }
    }
    return NULL;
}
pgsql_reader_ctx *pgsql_reader_at(pgpack_ctx *pgpack, uint32_t idx, pgpack_format format) {
    if (idx >= pgsql_result_count(pgpack)) {
        return NULL;// 类型不符时 count 为 0, 任何下标都被这条挡住
    }
    return _pgsql_reader_take(pgpack, idx, format);
}
void pgsql_reader_free(pgsql_reader_ctx *reader) {
    _pgpack_reader_free(reader); // 释放行数组与字段数组
    FREE(reader);
}
size_t pgsql_reader_size(pgsql_reader_ctx *reader) {
    return pgrow_arr_size(&reader->arr_rows);
}
void pgsql_reader_seek(pgsql_reader_ctx *reader, size_t pos) {
    if (pos >= pgrow_arr_size(&reader->arr_rows)) {
        return; // 目标位置超出范围，不移动游标
    }
    reader->index = (int32_t)pos;
}
int32_t pgsql_reader_eof(pgsql_reader_ctx *reader) {
    return (reader->index >= (int32_t)pgrow_arr_size(&reader->arr_rows)) ? 1 : 0;
}
void pgsql_reader_next(pgsql_reader_ctx *reader) {
    if (reader->index < (int32_t)pgrow_arr_size(&reader->arr_rows)) {
        reader->index++;
    }
}
pgpack_row *pgsql_reader_index(pgsql_reader_ctx *reader, int16_t index, pgpack_field **field) {
    if (reader->index >= (int32_t)pgrow_arr_size(&reader->arr_rows)
        || (index < 0 || index >= reader->field_count)) {
        return NULL;
    }
    pgpack_row *row = *pgrow_arr_at(&reader->arr_rows, reader->index);
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
// 8 个取值函数开头那三段（取行 → 类型 OID 白名单 → NULL 判定）收在这一处。
// 与 mysql_reader.c 的 _mysql_reader_row 同形，唯一差别是那边 NULL 判定排在类型判定之前。
// "row 非 NULL 就直接解引用 field" 靠 pgsql_reader_name 挡掉 fields == NULL，只在这里成立。
// 返回 NULL 时 err 已写好（ERR_FAILED=取不到/类型不符，1=字段是 NULL），调用方只管返自己的零值
static pgpack_row *_pgsql_reader_row(pgsql_reader_ctx *reader, const char *name,
                                     const int32_t *oids, int32_t noid,
                                     pgpack_field **field, int32_t *err) {
    pgpack_row *row = pgsql_reader_name(reader, name, field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    int32_t i;
    for (i = 0; i < noid; i++) {
        if (oids[i] == (*field)->type_oid) {
            break;
        }
    }
    if (i == noid) {// 字段类型不在白名单里
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    if (-1 == row->lens) {// NULL 值
        SET_PTR(err, 1);
        return NULL;
    }
    return row;
}
int32_t pgsql_reader_bool(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    static const int32_t _oids[] = { BOOLOID };
    pgpack_field *field;
    pgpack_row *row = _pgsql_reader_row(reader, name, _oids, (int32_t)ARRAY_SIZE(_oids), &field, err);
    if (NULL == row) {
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        // 文本格式：识别 t/true/y/yes/on/1 为真(不分大小写)。服务端输出恒为单字节 t/f，先走单字节
        if (1 == row->lens) {
            char c = (char)(row->val[0] | 0x20);
            return ('t' == c || 'y' == c || '1' == row->val[0]) ? 1 : 0;
        }
        static const pgsql_bool_word _pgsql_true[] = { { "true", 4 }, { "yes", 3 }, { "on", 2 } };
        for (int32_t i = 0; i < (int32_t)ARRAY_SIZE(_pgsql_true); i++) {
            if (_pgsql_true[i].n == row->lens
                && 0 == memcasecmp(row->val, _pgsql_true[i].s, (size_t)row->lens)) {
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
    static const int32_t _oids[] = { INT2OID, INT4OID, INT8OID };
    pgpack_field *field;
    pgpack_row *row = _pgsql_reader_row(reader, name, _oids, (int32_t)ARRAY_SIZE(_oids), &field, err);
    if (NULL == row) {
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        // 文本格式：判定与 mysql 侧共用 strtoi64，这里只负责写 err 和打日志
        int64_t val;
        if (ERR_OK != strtoi64(row->val, (size_t)row->lens, &val)) {
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
    return read_integer(row->val, (size_t)row->lens, 0, 1);
}
double pgsql_reader_double(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    static const int32_t _oids[] = { FLOAT4OID, FLOAT8OID };
    pgpack_field *field;
    pgpack_row *row = _pgsql_reader_row(reader, name, _oids, (int32_t)ARRAY_SIZE(_oids), &field, err);
    if (NULL == row) {
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        // 文本格式：空串 / 有残留字符 / 上溢的判定与 mysql 侧共用 strtod_s
        double val;
        if (ERR_OK != strtod_s(row->val, (size_t)row->lens, &val)) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0.0;
        }
        return val;
    }
    // 二进制格式：float4=4 字节、float8=8 字节，长度不符即拒绝
    int32_t expect = (FLOAT4OID == field->type_oid) ? 4 : 8;
    if (expect != row->lens) {
        SET_PTR(err, ERR_FAILED);
        return 0.0;
    }
    return (4 == expect) ? unpack_float(row->val, 0) : unpack_double(row->val, 0);
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
    static const int32_t _oids[] = { TEXTOID, VARCHAROID, BPCHAROID, NAMEOID, UNKNOWNOID };
    pgpack_field *field;
    pgpack_row *row = _pgsql_reader_row(reader, name, _oids, (int32_t)ARRAY_SIZE(_oids), &field, err);
    if (NULL == row) {
        return NULL;
    }
    // 文本/二进制格式均为 UTF-8 字节流，直接返回指针，不含 '\0' 结尾
    SET_PTR(lens, row->lens);
    return row->val;
}
const char *pgsql_reader_bytea(pgsql_reader_ctx *reader, const char *name, int32_t *lens, int32_t *err) {
    SET_PTR(lens, 0);
    SET_PTR(err, ERR_OK);
    static const int32_t _oids[] = { BYTEAOID };
    pgpack_field *field;
    pgpack_row *row = _pgsql_reader_row(reader, name, _oids, (int32_t)ARRAY_SIZE(_oids), &field, err);
    if (NULL == row) {
        return NULL;
    }
    // 二进制格式：原始字节；文本格式：'\x' 前缀 + 十六进制字符串（调用方自行解码）
    SET_PTR(lens, row->lens);
    return row->val;
}
// JDN 计算（与 PostgreSQL date2j 逻辑一致）：返回相对 PG 纪元（2000-01-01）的天数
static inline int32_t _pgsql_date_to_days(int32_t y, int32_t m, int32_t d) {
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
// s 里 pos 列出的下标全是十进制数字
static inline int32_t _pgsql_all_digits(const char *s, const uint8_t *pos, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        if ((uint32_t)((uint8_t)s[pos[i]] - '0') > 9) {
            return 0;
        }
    }
    return 1;
}
// 两位十进制数字，调用方已确认是数字
static inline int32_t _pgsql_dig2(const char *s) {
    return (s[0] - '0') * 10 + (s[1] - '0');
}
// "YYYY-MM-DD" 定宽快路径：s 至少 10 字节。字段量程与 _strptime 的 %Y-%m-%d 相同，
// 形状不符或越界返回 NULL 交回 _strptime 定夺，所以只会少走一趟 _strptime，不会多收或少收
static const char *_pgsql_date_fixed(const char *s, struct tm *dt) {
    static const uint8_t _pos[8] = { 0, 1, 2, 3, 5, 6, 8, 9 };
    if ('-' != s[4]
        || '-' != s[7]
        || !_pgsql_all_digits(s, _pos, (int32_t)ARRAY_SIZE(_pos))) {
        return NULL;
    }
    int32_t mon = _pgsql_dig2(s + 5);
    int32_t mday = _pgsql_dig2(s + 8);
    if (mon < 1 || mon > 12 || mday < 1 || mday > 31) {
        return NULL;
    }
    dt->tm_year = _pgsql_dig2(s) * 100 + _pgsql_dig2(s + 2) - 1900;
    dt->tm_mon = mon - 1;
    dt->tm_mday = mday;
    return s + 10;
}
// "YYYY-MM-DD HH:MM:SS" 定宽快路径：s 至少 19 字节，其余同 _pgsql_date_fixed
static const char *_pgsql_ts_fixed(const char *s, struct tm *dt) {
    static const uint8_t _pos[6] = { 11, 12, 14, 15, 17, 18 };
    if (' ' != s[10]
        || ':' != s[13]
        || ':' != s[16]
        || !_pgsql_all_digits(s, _pos, (int32_t)ARRAY_SIZE(_pos))
        || NULL == _pgsql_date_fixed(s, dt)) {
        return NULL;
    }
    int32_t hour = _pgsql_dig2(s + 11);
    int32_t min = _pgsql_dig2(s + 14);
    int32_t sec = _pgsql_dig2(s + 17);
    if (hour > 23 || min > 59 || sec > 61) {
        return NULL;
    }
    dt->tm_hour = hour;
    dt->tm_min = min;
    dt->tm_sec = sec;
    return s + 19;
}
// 将文本格式时间戳 "YYYY-MM-DD HH:MM:SS[.ffffff]" 解析为相对 PG 纪元的微秒数。
// 用 _strptime 而不是 sscanf 是为了逐字段量程校验：%d 什么都收，垃圾年份会算出垃圾天数还报成功。
// 已知收窄：年份只支持到 9999，而 PG 支持到 294276 AD，5 位及以上的年份解析失败。
// 由 test_pgsql_parse.c 的 test_pgsql_reader_temporal_text_range 钉住；_pgsql_days_from_text 同此
static int64_t _pgsql_usec_from_text(const char *s, int32_t slen, int32_t *err) {
    char tmp[48];
    if (ERR_OK != copy_bounded(s, (size_t)slen, tmp, sizeof(tmp), 1)) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    struct tm dt = { 0 };
    const char *end = (slen >= 19) ? _pgsql_ts_fixed(tmp, &dt) : NULL;
    if (NULL == end) {
        end = _strptime(tmp, "%Y-%m-%d %H:%M:%S", &dt);
    }
    if (NULL == end) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    // 小数秒 / " BC" / 时区偏移这三段 _strptime 表达不了：前两者没有对应转换符，
    // %z 在本仓库(TM_GMTOFF 从未定义)只吃掉时区文本、不保存偏移量，且它也不认 "+hh:mm:ss"
    uint32_t usec = parse_usec_frac(end);
    int32_t y = dt.tm_year + 1900;
    if (NULL != strstr(end, " BC")) {
        y = 1 - y;
    }
    int64_t days = _pgsql_date_to_days(y, dt.tm_mon + 1, dt.tm_mday);
    int64_t total = days * 86400000000LL
         + (int64_t)dt.tm_hour * 3600000000LL
         + (int64_t)dt.tm_min * 60000000LL
         + (int64_t)dt.tm_sec * 1000000LL
         + usec;
    // 偏移是 ±HH[:MM[:SS]]。上界只为挡住装不下 int 的位数,不代 PG 卡它自己的语义范围;
    // 不用 sscanf("%d:%d:%d")的理由同上面改用 _strptime 那条
    static const uint32_t _tz_max[3] = { 23, 59, 59 };
    uint32_t tz[3];
    int64_t offset;
    // 从日期时间之后起扫，不再按固定下标 11 跳过 "YYYY-MM-DD" 的两个减号——年份不是 4 位就跳错位
    for (const char *p = end; '\0' != *p; p++) {
        if ('+' == *p || '-' == *p) {
            if (0 == parse_colon_triple(p + 1, _tz_max, tz)) {
                SET_PTR(err, ERR_FAILED);
                return 0;
            }
            offset = ((int64_t)tz[0] * 3600 + (int64_t)tz[1] * 60 + (int64_t)tz[2]) * 1000000LL;
            total -= ('-' == *p) ? -offset : offset;
            break;
        }
    }
    return total;
}
// 将文本格式日期 "YYYY-MM-DD" 解析为相对 PG 纪元的天数；%Y 的年份上界见 _pgsql_usec_from_text
static int32_t _pgsql_days_from_text(const char *s, int32_t slen, int32_t *err) {
    char tmp[16];
    if (ERR_OK != copy_bounded(s, (size_t)slen, tmp, sizeof(tmp), 1)) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    struct tm dt = { 0 };
    const char *end = (slen >= 10) ? _pgsql_date_fixed(tmp, &dt) : NULL;
    if (NULL == end) {
        end = _strptime(tmp, "%Y-%m-%d", &dt);
    }
    if (NULL == end) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    int32_t y = dt.tm_year + 1900;
    if (NULL != strstr(end, " BC")) {
        y = 1 - y;
    }
    return _pgsql_date_to_days(y, dt.tm_mon + 1, dt.tm_mday);
}
int64_t pgsql_reader_timestamp(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    static const int32_t _oids[] = { TIMESTAMPOID, TIMESTAMPTZOID };
    pgpack_field *field;
    pgpack_row *row = _pgsql_reader_row(reader, name, _oids, (int32_t)ARRAY_SIZE(_oids), &field, err);
    if (NULL == row) {
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
    return (int64_t)read_be64(row->val);
}
int32_t pgsql_reader_date(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    static const int32_t _oids[] = { DATEOID };
    pgpack_field *field;
    pgpack_row *row = _pgsql_reader_row(reader, name, _oids, (int32_t)ARRAY_SIZE(_oids), &field, err);
    if (NULL == row) {
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
    return (int32_t)read_be32(row->val);
}
int32_t pgsql_reader_uuid(pgsql_reader_ctx *reader, const char *name, char uuid[16], int32_t *err) {
    SET_PTR(err, ERR_OK);
    static const int32_t _oids[] = { UUIDOID };
    pgpack_field *field;
    pgpack_row *row = _pgsql_reader_row(reader, name, _oids, (int32_t)ARRAY_SIZE(_oids), &field, err);
    if (NULL == row) {
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
    if (ERR_OK != uuid_fromstr(row->val, (size_t)row->lens, uuid)) {
        SET_PTR(err, ERR_FAILED);
        return ERR_FAILED;
    }
    return ERR_OK;
}
