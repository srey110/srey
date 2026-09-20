#include "protocol/mysql/mysql_reader.h"
#include "protocol/mysql/mysql_parse.h"
#include "protocol/mysql/mysql_utils.h"
#include "protocol/prots_pub.h"
#include "utils/strptime.h"

// 有符号与无符号整数读取共用同一份类型白名单：签名不同但"哪些列算整数"是同一条规则，
// 各留一份的话加一种整数类型要改两处，而没有任何东西把它们关联起来
static const uint8_t _int_types[] = { MYSQL_TYPE_LONGLONG, MYSQL_TYPE_LONG, MYSQL_TYPE_INT24,
                                      MYSQL_TYPE_SHORT, MYSQL_TYPE_YEAR, MYSQL_TYPE_TINY };

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
    return array_size(&reader->arr_rows);
}
void mysql_reader_seek(mysql_reader_ctx *reader, size_t pos) {
    if (pos >= array_size(&reader->arr_rows)) {
        return;
    }
    reader->index = (int32_t)pos;
}
int32_t mysql_reader_eof(mysql_reader_ctx *reader) {
    return (reader->index >= (int32_t)array_size(&reader->arr_rows)) ? 1 : 0;
}
void mysql_reader_next(mysql_reader_ctx *reader) {
    if (reader->index < (int32_t)array_size(&reader->arr_rows)) {
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
// 每个取值函数开头那三段（定位当前行 → NULL 判定 → 字段类型白名单）收在这里，
// types/ntype 是调用方允许的 enum_field_types 列表。
// 返回 NULL 时 err 已写好（1=该字段是 SQL NULL，ERR_FAILED=取不到或类型不符），调用方只管返自己的零值。
// NULL 判定排在类型判定之前，与 pgsql_reader 相反——那边先判类型；这里保持原有行为，
// 列值为 NULL 时不再多报一次类型不符
static mpack_row *_mysql_reader_row(mysql_reader_ctx *reader, const char *name,
                                    const uint8_t *types, int32_t ntype, int32_t *err) {
    if (reader->index >= (int32_t)array_size(&reader->arr_rows)) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    int32_t pos;
    mpack_field *column = _mysql_reader_field(reader, name, &pos);
    if (NULL == column) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    mpack_row *row = *(mpack_row **)(array_at(&reader->arr_rows, (uint32_t)reader->index));
    if (row[pos].nil) {
        SET_PTR(err, 1); // 1 表示该字段值为 NULL
        return NULL;
    }
    int32_t i;
    for (i = 0; i < ntype; i++) {
        if (types[i] == column->type) {
            break;
        }
    }
    if (i == ntype) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("does not match required data type.");
        return NULL;
    }
    return &row[pos];
}
int64_t mysql_reader_integer(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_row *row = _mysql_reader_row(reader, name, _int_types, (int32_t)ARRAY_SIZE(_int_types), err);
    if (NULL == row) {
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        // 文本协议：空串 / 含非数字 / 超量程的判定与 pgsql 侧共用 parse_int64_strict，
        // 这里只负责写 err 和打日志
        int64_t val;
        if (ERR_OK != parse_int64_strict(row->val.data, row->val.lens, &val)) {
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
            return unpack_integer(row->val.data, (int32_t)row->val.lens, 1, 1);
        }
    }
}
uint64_t mysql_reader_uinteger(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_row *row = _mysql_reader_row(reader, name, _int_types, (int32_t)ARRAY_SIZE(_int_types), err);
    if (NULL == row) {
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        // 文本协议：字段值为字符串，需转换为无符号整数。按 lens 直接解析，
        // 不再中转定长栈缓冲——原来的 strtoull 会把 "-1" 回绕成 UINT64_MAX 当合法值收下
        uint64_t val;
        if (ERR_OK != str2u64((const char *)row->val.data, row->val.lens, UINT64_MAX, &val)) {
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
            return unpack_integer(row->val.data, (int32_t)row->val.lens, 1, 0);
        }
    }
}
// 文本协议浮点解析公共逻辑：空串 / 有残留字符 / 上溢的判定全在 parse_double_strict 里，
// 与 pgsql 侧共用同一份（见 prots_pub.h），这里只负责写 err 和打日志
static inline double _mysql_reader_parse_text_float(mpack_row *row, int32_t *err) {
    double val;
    if (ERR_OK != parse_double_strict(row->val.data, row->val.lens, &val)) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("parse failed.");
        return 0.0;
    }
    return val;
}
float mysql_reader_float(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    static const uint8_t _types[] = { MYSQL_TYPE_FLOAT };
    mpack_row *row = _mysql_reader_row(reader, name, _types, (int32_t)ARRAY_SIZE(_types), err);
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
    static const uint8_t _types[] = { MYSQL_TYPE_DOUBLE };
    mpack_row *row = _mysql_reader_row(reader, name, _types, (int32_t)ARRAY_SIZE(_types), err);
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
    static const uint8_t _types[] = { MYSQL_TYPE_STRING, MYSQL_TYPE_VARCHAR, MYSQL_TYPE_VAR_STRING, MYSQL_TYPE_ENUM,
                                      MYSQL_TYPE_SET, MYSQL_TYPE_LONG_BLOB, MYSQL_TYPE_MEDIUM_BLOB, MYSQL_TYPE_BLOB,
                                      MYSQL_TYPE_TINY_BLOB, MYSQL_TYPE_GEOMETRY, MYSQL_TYPE_BIT, MYSQL_TYPE_DECIMAL,
                                      MYSQL_TYPE_NEWDECIMAL, MYSQL_TYPE_JSON };
    mpack_row *row = _mysql_reader_row(reader, name, _types, (int32_t)ARRAY_SIZE(_types), err);
    if (NULL == row) {
        return NULL;
    }
    *lens = row->val.lens;
    return row->val.data;
}
int64_t mysql_reader_datetime(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    static const uint8_t _types[] = { MYSQL_TYPE_DATE, MYSQL_TYPE_DATETIME, MYSQL_TYPE_DATETIME2, MYSQL_TYPE_TIMESTAMP,
                                      MYSQL_TYPE_TIMESTAMP2 };
    mpack_row *row = _mysql_reader_row(reader, name, _types, (int32_t)ARRAY_SIZE(_types), err);
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
        errno = 0;
        time_t ts = mktime(&dt);
        if ((time_t)-1 == ts && 0 != errno) {
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
        errno = 0;
        time_t ts = mktime(&dt);
        if ((time_t)-1 == ts && 0 != errno) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        uint32_t usec = (11 == row->val.lens) ? (uint32_t)binary_get_integer(&breader, 4, 1) : 0;
        return (int64_t)ts * 1000000LL + usec;
    }
}
int32_t mysql_reader_time(mysql_reader_ctx *reader, const char *name, struct tm *time, uint32_t *usec, int32_t *err) {
    SET_PTR(err, ERR_OK);
    static const uint8_t _types[] = { MYSQL_TYPE_TIME, MYSQL_TYPE_TIME2 };
    // 出参先清零再取行:取不到(列为 SQL NULL 或类型不符)时也给确定值,同族的 integer/double 一样
    *time = (struct tm) { 0 };
    *usec = 0;
    mpack_row *row = _mysql_reader_row(reader, name, _types, (int32_t)ARRAY_SIZE(_types), err);
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
