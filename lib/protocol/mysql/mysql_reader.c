#include "protocol/mysql/mysql_reader.h"
#include "protocol/mysql/mysql_parse.h"
#include "protocol/mysql/mysql_utils.h"
#include "protocol/prots_pub.h"
#include "utils/strptime.h"

// 列类型 → 取值分组(MYSQL_CLS_*，定义见 mysql_reader.h)
static const uint8_t _mysql_type_cls[256] = {
    [MYSQL_TYPE_LONGLONG] = MYSQL_CLS_INT, [MYSQL_TYPE_LONG] = MYSQL_CLS_INT, [MYSQL_TYPE_INT24] = MYSQL_CLS_INT,
    [MYSQL_TYPE_SHORT] = MYSQL_CLS_INT, [MYSQL_TYPE_YEAR] = MYSQL_CLS_INT, [MYSQL_TYPE_TINY] = MYSQL_CLS_INT,
    [MYSQL_TYPE_FLOAT] = MYSQL_CLS_FLOAT,
    [MYSQL_TYPE_DOUBLE] = MYSQL_CLS_DOUBLE,
    [MYSQL_TYPE_STRING] = MYSQL_CLS_STRING, [MYSQL_TYPE_VARCHAR] = MYSQL_CLS_STRING, [MYSQL_TYPE_VAR_STRING] = MYSQL_CLS_STRING,
    [MYSQL_TYPE_ENUM] = MYSQL_CLS_STRING, [MYSQL_TYPE_SET] = MYSQL_CLS_STRING, [MYSQL_TYPE_LONG_BLOB] = MYSQL_CLS_STRING,
    [MYSQL_TYPE_MEDIUM_BLOB] = MYSQL_CLS_STRING, [MYSQL_TYPE_BLOB] = MYSQL_CLS_STRING, [MYSQL_TYPE_TINY_BLOB] = MYSQL_CLS_STRING,
    [MYSQL_TYPE_GEOMETRY] = MYSQL_CLS_STRING, [MYSQL_TYPE_BIT] = MYSQL_CLS_STRING, [MYSQL_TYPE_DECIMAL] = MYSQL_CLS_STRING,
    [MYSQL_TYPE_NEWDECIMAL] = MYSQL_CLS_STRING, [MYSQL_TYPE_JSON] = MYSQL_CLS_STRING,
    [MYSQL_TYPE_DATE] = MYSQL_CLS_DATETIME, [MYSQL_TYPE_DATETIME] = MYSQL_CLS_DATETIME, [MYSQL_TYPE_DATETIME2] = MYSQL_CLS_DATETIME,
    [MYSQL_TYPE_TIMESTAMP] = MYSQL_CLS_DATETIME, [MYSQL_TYPE_TIMESTAMP2] = MYSQL_CLS_DATETIME,
    [MYSQL_TYPE_TIME] = MYSQL_CLS_TIME, [MYSQL_TYPE_TIME2] = MYSQL_CLS_TIME
};

#if defined(OS_WIN)
// 同 mktime(dt->tm_isdst 须为 -1)。Windows 直接调 mktime、不走偏移缓存：UCRT 的 mktime 比快路径要做的几次 localtime_s 便宜
static int32_t _mysql_mktime(mysql_reader_ctx *reader, struct tm *dt, time_t *ts) {
    (void)reader;
    errno = 0;
    time_t t = mktime(dt);
    if ((time_t)-1 == t && 0 != errno) {
        return ERR_FAILED;
    }
    *ts = t;
    return ERR_OK;
}
#else
// 公历日期时间按 UTC 算出的秒数(days_from_civil)，字段不查量程
static inline int64_t _mysql_civil_secs(const struct tm *t) {
    int64_t y = (int64_t)t->tm_year + 1900;
    int64_t m = (int64_t)t->tm_mon + 1;
    y -= (m <= 2) ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t->tm_mday - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + (int64_t)t->tm_hour * 3600 + (int64_t)t->tm_min * 60 + t->tm_sec;
}
// t 前后 26 小时的本地偏移都是 off 时返 1，探测时刻超出 time_t 的范围返 0。前提是任意两个本地偏移相差不超过 26 小时
// (UTC-12 ~ UTC+14)、两次跳变相隔超过 2 天：此时 t 的本地时间只对应 t 一个瞬间
static inline int32_t _mysql_tz_stable(time_t t, int64_t off) {
    const int64_t win = 26 * 3600;
    struct tm lt;
    time_t p;
    int64_t probe = (int64_t)t - win;
    for (int32_t i = 0; i < 2; i++, probe += 2 * win) {
        p = (time_t)probe;
        if ((int64_t)p != probe
            || 0 != LOCALTIME(&p, &lt)
            || _mysql_civil_secs(&lt) - probe != off) {
            return 0;
        }
    }
    return 1;
}
// 同 mktime(dt->tm_isdst 须为 -1)，Windows 以外的平台。先按缓存的本地偏移算候选瞬间、转回本地时间逐字段核对，
// 对得上且前后 26 小时没有跳变才用；对不上(偏移变了、字段越界要归一、超出 time_t 的范围)或附近有跳变(秋季重叠、空档)
// 都调 mktime 并缓存它的偏移。结果与 mktime 逐次相同只在 _mysql_tz_stable 的前提下成立。
// 1900 年以前一律走 mktime：macOS 的 mktime 在那里返回 -1
static int32_t _mysql_mktime(mysql_reader_ctx *reader, struct tm *dt, time_t *ts) {
    struct tm lt;
    time_t t;
    int64_t cand;
    int64_t base = _mysql_civil_secs(dt);
    for (int32_t i = 0; i < reader->ntzoff && dt->tm_year >= 0; i++) {
        cand = base - reader->tzoff[i];
        t = (time_t)cand;
        if ((int64_t)t == cand
            && 0 == LOCALTIME(&t, &lt)
            && lt.tm_sec == dt->tm_sec && lt.tm_min == dt->tm_min && lt.tm_hour == dt->tm_hour
            && lt.tm_mday == dt->tm_mday && lt.tm_mon == dt->tm_mon && lt.tm_year == dt->tm_year) {
            if (_mysql_tz_stable(t, reader->tzoff[i])) {
                *ts = t;
                return ERR_OK;
            }
            break;
        }
    }
    errno = 0;
    t = mktime(dt);
    if ((time_t)-1 == t && 0 != errno) {
        return ERR_FAILED;
    }
    *ts = t;
    if (0 == LOCALTIME(&t, &lt)) {
        reader->tzoff[1] = reader->tzoff[0];
        reader->tzoff[0] = (int32_t)(_mysql_civil_secs(&lt) - (int64_t)t);
        reader->ntzoff = (reader->ntzoff < 2) ? reader->ntzoff + 1 : 2;
    }
    return ERR_OK;
}
#endif
mysql_reader_ctx *mysql_reader_init(mpack_ctx *mpack) {
    if ((MPACK_QUERY != mpack->pack_type && MPACK_STMT_EXECUTE != mpack->pack_type)
        || NULL == mpack->pack) {
        return NULL;
    }
    mysql_reader_ctx *reader = mpack->pack;
    // 将所有权从 mpack 转移给调用方，避免重复释放
    mpack->pack = NULL;
    mpack->_free_mpack = NULL;
    return reader;
}
void mysql_reader_free(mysql_reader_ctx *reader) {
    _mpack_reader_free(reader);
    FREE(reader);
}
size_t mysql_reader_size(mysql_reader_ctx *reader) {
    return mrow_arr_size(&reader->arr_rows);
}
void mysql_reader_seek(mysql_reader_ctx *reader, size_t pos) {
    if (pos >= mrow_arr_size(&reader->arr_rows)) {
        return;
    }
    reader->index = (int32_t)pos;
}
int32_t mysql_reader_eof(mysql_reader_ctx *reader) {
    return (reader->index >= (int32_t)mrow_arr_size(&reader->arr_rows)) ? 1 : 0;
}
void mysql_reader_next(mysql_reader_ctx *reader) {
    if (reader->index < (int32_t)mrow_arr_size(&reader->arr_rows)) {
        reader->index++;
    }
}
// 根据字段名在列描述数组中查找对应字段，返回字段指针并输出列索引
static mpack_field *_mysql_reader_field(mysql_reader_ctx *reader, const char *name, int32_t *pos) {
    size_t nlens = strlen(name);
    for (int32_t i = 0; i < reader->field_count; i++) {
        if (buf_compare(&reader->fields[i].name, name, nlens)) {
            *pos = i;
            return &reader->fields[i];
        }
    }
    return NULL;
}
uint8_t mysql_reader_cls(mysql_reader_ctx *reader, const char *name) {
    int32_t pos;
    mpack_field *column = _mysql_reader_field(reader, name, &pos);
    return (NULL == column) ? 0 : _mysql_type_cls[column->type];
}
int32_t mysql_reader_unsigned(mysql_reader_ctx *reader, const char *name) {
    int32_t pos;
    mpack_field *column = _mysql_reader_field(reader, name, &pos);
    return (NULL != column && 0 != (column->flags & MYSQL_UNSIGNED_FLAG)) ? 1 : 0;
}
int32_t mysql_reader_isnull(mysql_reader_ctx *reader, const char *name) {
    int32_t pos;
    if (reader->index >= (int32_t)mrow_arr_size(&reader->arr_rows)
        || NULL == _mysql_reader_field(reader, name, &pos)) {
        return 0;
    }
    return (*mrow_arr_at(&reader->arr_rows, reader->index))[pos].nil ? 1 : 0;
}
// 每个取值函数开头那三段（定位当前行 → NULL 判定 → 字段类型白名单）收在这里，
// cls 是调用方收的列类型组(MYSQL_CLS_*，见 _mysql_type_cls)。
// 返回 NULL 时 err 已写好（1=该字段是 SQL NULL，ERR_FAILED=取不到或类型不符），调用方只管返自己的零值。
// NULL 判定排在类型判定之前，与 pgsql_reader 相反——那边先判类型；
// 这里先判 NULL，列值为 NULL 时不再多报一次类型不符
static mpack_row *_mysql_reader_row(mysql_reader_ctx *reader, const char *name,
                                    uint8_t cls, int32_t *err) {
    if (reader->index >= (int32_t)mrow_arr_size(&reader->arr_rows)) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    int32_t pos;
    mpack_field *column = _mysql_reader_field(reader, name, &pos);
    if (NULL == column) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    mpack_row *row = *mrow_arr_at(&reader->arr_rows, reader->index);
    if (row[pos].nil) {
        SET_PTR(err, 1); // 1 表示该字段值为 NULL
        return NULL;
    }
    if (0 == (_mysql_type_cls[column->type] & cls)) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("does not match required data type.");
        return NULL;
    }
    return &row[pos];
}
int64_t mysql_reader_integer(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_row *row = _mysql_reader_row(reader, name, MYSQL_CLS_INT, err);
    if (NULL == row) {
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        // 文本协议：空串 / 含非数字 / 超量程的判定与 pgsql 侧共用 strtoi64，
        // 这里只负责写 err 和打日志
        int64_t val;
        if (ERR_OK != strtoi64(row->val.data, row->val.lens, &val)) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0;
        }
        return val;
    } else {
        // 二进制协议：字段值为原始二进制整数
        if (sizeof(int8_t) == row->val.lens) {
            return (int8_t)(((char *)row->val.data)[0]);// 无符号 char 平台须显式转 int8_t 才能保留 TINYINT 负值
        } else {
            return read_integer(row->val.data, row->val.lens, 1, 1);
        }
    }
}
uint64_t mysql_reader_uinteger(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_row *row = _mysql_reader_row(reader, name, MYSQL_CLS_INT, err);
    if (NULL == row) {
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        // 文本协议：字段值为字符串，需转换为无符号整数。按 lens 直接解析
        uint64_t val;
        if (ERR_OK != strtou64((const char *)row->val.data, row->val.lens, UINT64_MAX, &val)) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0;
        }
        return val;
    } else {
        // 二进制协议：字段值为原始二进制无符号整数
        if (sizeof(uint8_t) == row->val.lens) {
            return (uint8_t)(((char *)row->val.data)[0]);
        } else {
            return read_integer(row->val.data, row->val.lens, 1, 0);
        }
    }
}
// 文本协议浮点解析公共逻辑：空串 / 有残留字符 / 上溢的判定全在 strtod_s 里，
// 与 pgsql 侧共用同一份（见 prots_pub.h），这里只负责写 err 和打日志
static inline double _mysql_reader_parse_text_float(mpack_row *row, int32_t *err) {
    double val;
    if (ERR_OK != strtod_s(row->val.data, row->val.lens, &val)) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("parse failed.");
        return 0.0;
    }
    return val;
}
float mysql_reader_float(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_row *row = _mysql_reader_row(reader, name, MYSQL_CLS_FLOAT, err);
    if (NULL == row) {
        return 0.0f;
    }
    if (MPACK_QUERY == reader->pack_type) {
        return (float)_mysql_reader_parse_text_float(row, err);
    } else {
        if (sizeof(float) != row->val.lens) {
            SET_PTR(err, ERR_FAILED);
            return 0.0f;
        }
        return unpack_float(row->val.data, 1);
    }
}
double mysql_reader_double(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_row *row = _mysql_reader_row(reader, name, MYSQL_CLS_DOUBLE, err);
    if (NULL == row) {
        return 0.0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        return _mysql_reader_parse_text_float(row, err);
    } else {
        if (sizeof(double) != row->val.lens) {
            SET_PTR(err, ERR_FAILED);
            return 0.0;
        }
        return unpack_double(row->val.data, 1);
    }
}
char *mysql_reader_string(mysql_reader_ctx *reader, const char *name, size_t *lens, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_row *row = _mysql_reader_row(reader, name, MYSQL_CLS_STRING, err);
    if (NULL == row) {
        return NULL;
    }
    *lens = row->val.lens;
    return row->val.data;
}
int64_t mysql_reader_datetime(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_row *row = _mysql_reader_row(reader, name, MYSQL_CLS_DATETIME, err);
    if (NULL == row) {
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        char tmp[48];
        if (ERR_OK != copy_bounded(row->val.data, row->val.lens, tmp, sizeof(tmp), 1)) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        // 零日期 0000-00-00 是非严格 sql_mode 下的合法值，进 _strptime 之前先认掉：二进制协议
        // 按 0 + err=ERR_OK 返回，而 %m 卡 1..12 会判成解析失败，同值两条路径会分叉。
        // 折成 0 后与真实的 1970-01-01 分不开，现有契约留不出第四种状态
        if (row->val.lens >= 10
            && 0 == memcmp(tmp, "0000-00-00", 10)) {
            return 0;
        }
        struct tm dt = { 0 };
        dt.tm_isdst = -1;// 由 mktime 依日期/本地时区自行判定夏令时，否则 DST 期恒按标准时解释偏 1 小时
        // 用 _strptime 而不是 sscanf("%d-...")：要的是逐字段量程校验，%d 什么都收，越界值会被
        // mktime 静默归一。日期与时间分两段读(DATE 列只有日期)，但日期后面还有东西就必须是完整
        // 时间——否则 "2024-05-21 24:00:00" 会被当合法 DATE 收下、时间整段丢掉
        const char *end = _strptime(tmp, "%Y-%m-%d", &dt);
        if (NULL == end) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        if ('\0' != *end) {
            end = _strptime(end, " %H:%M:%S", &dt);
            if (NULL == end) {
                SET_PTR(err, ERR_FAILED);
                return 0;
            }
        }
        uint32_t usec = parse_usec_frac(end);
        time_t ts;
        if (ERR_OK != _mysql_mktime(reader, &dt, &ts)) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        return (int64_t)ts * 1000000LL + usec;
    } else {
        // 二进制协议：长度前缀 0=全零日期时间 4=仅日期 7=日期+时间 11=含微秒
        if (0 == row->val.lens) {
            return 0;
        }
        if (4 != row->val.lens && 7 != row->val.lens && 11 != row->val.lens) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        struct tm dt = { 0 };
        dt.tm_isdst = -1;// 由 mktime 依日期/本地时区自行判定夏令时，否则 DST 期恒按标准时解释偏 1 小时
        binary_ctx breader;
        binary_init_read(&breader, row->val.data, row->val.lens);
        dt.tm_year = (int32_t)binary_get_integer(&breader, 2, 1) - 1900;
        dt.tm_mon = (int32_t)binary_get_int8(&breader) - 1;
        dt.tm_mday = (int32_t)binary_get_int8(&breader);
        if (row->val.lens >= 7) {
            dt.tm_hour = (int32_t)binary_get_int8(&breader);
            dt.tm_min = (int32_t)binary_get_int8(&breader);
            dt.tm_sec = (int32_t)binary_get_int8(&breader);
        }
        time_t ts;
        if (ERR_OK != _mysql_mktime(reader, &dt, &ts)) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        uint32_t usec = (11 == row->val.lens) ? (uint32_t)binary_get_integer(&breader, 4, 1) : 0;
        return (int64_t)ts * 1000000LL + usec;
    }
}
int32_t mysql_reader_time(mysql_reader_ctx *reader, const char *name, struct tm *time, uint32_t *usec, int32_t *err) {
    SET_PTR(err, ERR_OK);
    // 出参先清零再取行:取不到(列为 SQL NULL 或类型不符)时也给确定值,同族的 integer/double 一样
    *time = (struct tm) { 0 };
    *usec = 0;
    mpack_row *row = _mysql_reader_row(reader, name, MYSQL_CLS_TIME, err);
    if (NULL == row) {
        return 0;
    }
    int32_t is_negative = 0;
    if (MPACK_QUERY == reader->pack_type) {
        // TIME 文本是 [-]HHH:MM:SS[.frac],量程 ±838:59:59,三段必须齐。
        // 不用 sscanf("%d:%d:%d")的理由同上面 mysql_reader_datetime 改用 _strptime 那条
        static const uint32_t _hms_max[3] = { 838, 59, 59 };
        uint32_t hms[3];
        char tmp[48];
        if (ERR_OK != copy_bounded(row->val.data, row->val.lens, tmp, sizeof(tmp), 1)) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        char *p = tmp;
        if ('-' == *p) {
            is_negative = 1;
            p++;
        }
        if (3 != parse_colon_triple(p, _hms_max, hms)) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        *usec = parse_usec_frac(p);
        time->tm_mday = (int32_t)(hms[0] / 24);
        time->tm_hour = (int32_t)(hms[0] % 24);
        time->tm_min = (int32_t)hms[1];
        time->tm_sec = (int32_t)hms[2];
    } else {
        // 二进制协议：长度前缀 0=零时间 8=天+时分秒 12=含微秒
        if (0 == row->val.lens) {
            return 0;
        }
        if (8 != row->val.lens && 12 != row->val.lens) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        binary_ctx breader;
        binary_init_read(&breader, row->val.data, row->val.lens);
        is_negative = (int32_t)binary_get_int8(&breader);
        time->tm_mday = (int32_t)binary_get_integer(&breader, 4, 1);
        time->tm_hour = (int32_t)binary_get_int8(&breader);
        time->tm_min = (int32_t)binary_get_int8(&breader);
        time->tm_sec = (int32_t)binary_get_int8(&breader);
        if (12 == row->val.lens) {
            *usec = (uint32_t)binary_get_integer(&breader, 4, 1);
        }
    }
    return is_negative;
}
