#include "protocol/mysql/mysql_parse.h"
#include "protocol/mysql/mysql.h"
#include "protocol/mysql/mysql_utils.h"
#include "protocol/prots_pub.h"
#include "event/evpub.h"
#include "utils/utils.h"

// 结果集解析状态：字段描述阶段 / 行数据阶段
typedef enum rst_status {
    RST_FIELD = 0x01,  // 正在解析列字段描述
    RST_ROW            // 正在解析行数据
}rst_status;
// 预处理语句准备响应解析状态：参数字段阶段 / 结果集字段阶段
typedef enum stmt_prepare_status {
    STMT_PREPARE_PARAMS = 0x01, // 正在解析参数字段描述
    STMT_PREPARE_FIELD          // 正在解析结果集字段描述
}stmt_prepare_status;
// EOF 包的三种结局。截断必须与"还有结果集"分开：行阶段把后者当成功交付，
// 两者共用一个失败码就会让残缺结果集也被当成"收完了，后面还有"
typedef enum eof_final {
    EOF_FINAL_DONE = 0, // 本次结果集到此结束
    EOF_FINAL_MORE,     // 服务端还有结果集要发，已置 PROT_MOREDATA
    EOF_FINAL_BROKEN    // EOF 包本身截断，按协议错误处理
}eof_final;

// 读取当前 offset 处首字节（不前进），用于响应包类型分派
static inline uint8_t _mysql_peek(binary_ctx *breader) {
    return (uint8_t)(binary_at(breader, breader->offset)[0]);
}
// 当前是否为结果集/定义段的终止包：首字节 0xfe 且包长 < 9（防与 8 字节 lenenc 值混淆）。
// 协商了 CLIENT_DEPRECATE_EOF 时终止包是 0xfe 头的 OK 包，可带 info / session track 超过 9 字节，不限长：
// 0xfe 开头的行要求首列值长 >= 2^24，已被 _mysql_payload 的 16MB 续传拒收挡住
static inline int32_t _mysql_is_eof_packet(mysql_ctx *mysql, binary_ctx *breader) {
    return MYSQL_EOF == _mysql_peek(breader)
        && (breader->size < 9 || BIT_CHECK(mysql->client.caps, CLIENT_DEPRECATE_EOF));
}
// 解析 MySQL 数据包头部（4 字节），输出 payload 长度，包头留在 buf 里；数据不足时返回 ERR_FAILED
static inline int32_t _mysql_head(mysql_ctx *mysql, buffer_ctx *buf, size_t *payload_lens) {
    size_t size = buffer_size(buf);
    if (size < MYSQL_HEAD_LENS) {
        return ERR_FAILED;
    }
    char head[MYSQL_HEAD_LENS];
    ASSERTAB(sizeof(head) == buffer_copyout(buf, 0, head, sizeof(head)), "copy buffer failed.");
    *payload_lens = (size_t)unpack_integer(head, 3, 1, 0);
    if (size < *payload_lens + sizeof(head)) {
        return ERR_FAILED;
    }
    mysql->id = (uint8_t)head[3];
    return ERR_OK;
}
// 结果集的列定义包与行包都从 reader 的块链切，随 reader 一起还，包头连同 payload 一次取进块；行包块尾多分行数组：
// [包头][payload][补齐到 8][mpack_row × 列数]。其余阶段与没有列的行阶段返回 NULL，包单独分配。
// STMT_PREPARE 的两个阶段与 RST_FIELD / RST_ROW 同值，须再判命令
static inline mem_arena *_mpack_arena(mysql_ctx *mysql, size_t *extra) {
    if ((RST_FIELD != mysql->parse_status && RST_ROW != mysql->parse_status)
        || (MYSQL_QUERY != mysql->cur_cmd && MYSQL_EXECUTE != mysql->cur_cmd)) {
        return NULL;
    }
    mysql_reader_ctx *reader = mysql->mpack->pack;
    if (RST_FIELD == mysql->parse_status) {
        *extra = 0;
        return &reader->arena;
    }
    if (reader->field_count <= 0) {
        return NULL;
    }
    *extra = sizeof(mpack_row) * (size_t)reader->field_count;
    return &reader->arena;
}
char *_mysql_payload(mysql_ctx *mysql, buffer_ctx *buf, size_t *payload_lens, int32_t *status) {
    if (ERR_OK != _mysql_head(mysql, buf, payload_lens)) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    // payload==0xffffff(INT3_MAX) 按 MySQL 协议恒表示服务端还有续传包(即便本包恰好 16MB-1)，
    // 本实现不支持拼接多物理包（同 _mysql_set_payload_lens 发送侧限制），报协议错误断连，
    // 避免把这个 16MB 块误当完整 payload 交付解析（行数据会在边界截断、长度字段错位）
    if (INT3_MAX == *payload_lens) {
        LOG_ERROR("mysql response requires 16MB packet continuation, unsupported.");
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    if (0 == *payload_lens) {
        LOG_ERROR("mysql zero-length packet.");
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    size_t extra;
    size_t skip = 0;
    size_t whole = *payload_lens;
    char *blk;
    mem_arena *arena = _mpack_arena(mysql, &extra);
    if (NULL == arena) {
        ASSERTAB(MYSQL_HEAD_LENS == buffer_drain(buf, MYSQL_HEAD_LENS), "drain buffer failed.");
        MALLOC(blk, whole);
    } else {
        skip = MYSQL_HEAD_LENS;
        whole += MYSQL_HEAD_LENS;
        blk = mem_arena_alloc(arena, ROUND_UP(whole, 8) + extra);
    }
    ASSERTAB(whole == buffer_remove(buf, blk, whole), "copy buffer failed.");
    return blk + skip;
}
char *_mysql_payload_first(mysql_ctx *mysql, buffer_ctx *buf, char *stk, size_t cap,
                           size_t *payload_lens, int32_t *status) {
    char head[MYSQL_HEAD_LENS];
    size_t lens;
    size_t size = buffer_size(buf);
    if (size >= MYSQL_HEAD_LENS) {
        ASSERTAB(sizeof(head) == buffer_copyout(buf, 0, head, sizeof(head)), "copy buffer failed.");
        lens = (size_t)unpack_integer(head, 3, 1, 0);
        if (lens > 0
            && lens <= cap - MYSQL_HEAD_LENS
            && size >= lens + MYSQL_HEAD_LENS) {
            mysql->id = (uint8_t)head[3];
            *payload_lens = lens;
            ASSERTAB(lens == buffer_copyout(buf, MYSQL_HEAD_LENS, stk + MYSQL_HEAD_LENS, lens), "copy buffer failed.");
            ASSERTAB(lens + MYSQL_HEAD_LENS == buffer_drain(buf, lens + MYSQL_HEAD_LENS), "drain buffer failed.");
            return stk + MYSQL_HEAD_LENS;
        }
    }
    return _mysql_payload(mysql, buf, payload_lens, status);
}
// OK 包尾部的 session-state-change：服务端切换当前库（USE / COM_INIT_DB / 存储过程内切库）都经此回带，
// 是 client.database 的权威来源。SERVER_SESSION_STATE_CHANGED 只会由接受了 CLIENT_SESSION_TRACK 的
// 服务端置位，故该位本身即可判定尾部是 lenenc 布局，不必再看协商结果；老服务端不置位,尾部整段跳过。
// 本段只是可选信息，任何不自洽都当"没带"静默放弃而不是拖垮进程：读长度一律走
// _mysql_get_lenenc（缓冲不够它返 ERR_FAILED），读出来的长度再与剩余字节比过才用
static void _mpack_ok_track(mysql_ctx *mysql, binary_ctx *breader) {
    int32_t rtn;
    uint64_t lens = _mysql_get_lenenc(breader, &rtn);
    if (ERR_OK != rtn
        || !binary_have(breader, lens)) {
        return;
    }
    binary_get_skip(breader, (size_t)lens);
    uint64_t total = _mysql_get_lenenc(breader, &rtn);
    if (ERR_OK != rtn
        || !binary_have(breader, total)) {
        return;
    }
    size_t end = breader->offset + (size_t)total;
    size_t next;
    uint64_t dlens;
    uint8_t type;
    char *name;
    while (breader->offset < end) {
        type = binary_get_uint8(breader);
        dlens = _mysql_get_lenenc(breader, &rtn);
        if (ERR_OK != rtn
            || breader->offset > end
            || dlens > (uint64_t)(end - breader->offset)) {
            return;
        }
        next = breader->offset + (size_t)dlens;
        if (SESSION_TRACK_SCHEMA == type) {
            lens = _mysql_get_lenenc(breader, &rtn);
            if (ERR_OK != rtn
                || breader->offset > next
                || lens > (uint64_t)(next - breader->offset)) {
                return;
            }
            name = binary_get_binary(breader, (size_t)lens);
            if (ERR_OK != copy_bounded(name, (size_t)lens, mysql->client.database,
                                              sizeof(mysql->client.database), 1)) {
                LOG_ERROR("mysql tracked schema exceeds %zu bytes: %"PRIu64", keep the old one.",
                          sizeof(mysql->client.database) - 1, lens);
            }
        }
        binary_offset(breader, next);
    }
}
// 读 OK 包体：两个计数与 status_flags 经出参回带(不要的传 NULL)，不落 ctx；带 session track 时顺带更新当前库
static int32_t _mpack_ok_parse(mysql_ctx *mysql, binary_ctx *breader,
    uint64_t *affected_rows, uint64_t *last_id, int16_t *status_flags) {
    int32_t _rtn;
    uint64_t affected = _mysql_get_lenenc(breader, &_rtn);
    if (ERR_OK != _rtn) {
        return ERR_FAILED;
    }
    uint64_t lastid = _mysql_get_lenenc(breader, &_rtn);
    if (ERR_OK != _rtn) {
        return ERR_FAILED;
    }
    if (!binary_have(breader, 4)) {// status_flags(2) + warnings(2)
        return ERR_FAILED;
    }
    int16_t flags = (int16_t)binary_get_integer(breader, 2, 1);
    binary_get_skip(breader, 2);// warnings：本库不用，只推进读位置
    if (BIT_CHECK(flags, SERVER_SESSION_STATE_CHANGED)) {
        _mpack_ok_track(mysql, breader);
    }
    binary_get_skip(breader, binary_remain(breader));
    SET_PTR(affected_rows, affected);
    SET_PTR(last_id, lastid);
    SET_PTR(status_flags, flags);
    return ERR_OK;
}
// 解析 OK 响应包，更新 mysql->last_id 和 mysql->affected_rows。
// status_flags 回带给调用方，只关心多结果集的那条路要它，其余传 NULL。
// 两个计数落到 ctx 之前先攒在局部量里：中途截断时不留下半套值
static int32_t _mpack_ok(mysql_ctx *mysql, binary_ctx *breader, int16_t *status_flags) {
    uint64_t affected;
    uint64_t lastid;
    if (ERR_OK != _mpack_ok_parse(mysql, breader, &affected, &lastid, status_flags)) {
        return ERR_FAILED;
    }
    mysql->last_id = (int64_t)lastid;
    mysql->affected_rows = (int64_t)affected;
    return ERR_OK;
}
// 解析 EOF 响应包，读取警告数和状态标志
// warnings(2) + status_flags(2)，读之前先比剩余字节；截断的包判失败而不是撞断言
static int32_t _mpack_eof(binary_ctx *breader, int16_t *status_flags) {
    if (!binary_have(breader, 4)) {
        return ERR_FAILED;
    }
    binary_get_skip(breader, 2);// warnings：本库不用，只推进读位置
    *status_flags = (int16_t)binary_get_integer(breader, 2, 1);
    return ERR_OK;
}
// ERR 包体：error_code(2) + '#' + sql_state(5) + 错误串。各段读之前都要比剩余字节——
// 报文长度由对端决定，截断的包只该当"没带"而不是撞上 binary_get_* 的断言把进程 abort
void _mpack_err(mysql_ctx *mysql, binary_ctx *breader) {
    if (!binary_have(breader, 2)) {
        mysql->error_code = 0;
        mysql->error_msg[0] = '\0';
        return;
    }
    mysql->error_code = (int16_t)binary_get_integer(breader, 2, 1);
    // sql_state 段只在 CLIENT_PROTOCOL_41 协商之后才有，握手前的 ERR(1040/1129/1130)不带它，
    // 故按 '#' 标记判定而不是无条件跳 6 字节——跳错了就从错误正文里啃掉六个字符
    if (binary_have(breader, 6)
        && '#' == *binary_at(breader, breader->offset)) {
        binary_get_skip(breader, 6);//sql_state_marker sql_state
    }
    size_t mlens = binary_remain(breader);
    if (mlens > 0) {
        copy_bounded(binary_get_binary(breader, mlens), mlens,
                     mysql->error_msg, sizeof(mysql->error_msg), 0);
    } else {
        mysql->error_msg[0] = '\0';
    }
}
// 分配并初始化一个新的 mpack_ctx
static inline mpack_ctx *_mpack_new(void) {
    mpack_ctx *mpack;
    CALLOC(mpack, 1, sizeof(mpack_ctx));
    return mpack;
}
// 解析简单命令响应（COM_INIT_DB / COM_PING / COM_STMT_RESET）：首字节区分 OK / ERR
static mpack_ctx *_mpack_simple_response(mysql_ctx *mysql, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = _mpack_new();
    if (MYSQL_OK == binary_get_uint8(breader)) {
        mpack->pack_type = MPACK_OK;
        // COM_INIT_DB / COM_PING / COM_STMT_RESET 不会有后续结果集，status_flags 无人问
        if (ERR_OK != _mpack_ok(mysql, breader, NULL)) {
            BIT_SET(*status, PROT_ERROR);
            _mysql_pkfree(mpack);
            return NULL;
        }
    } else {
        mpack->pack_type = MPACK_ERR;
        _mpack_err(mysql, breader);
    }
    mysql->cur_cmd = 0;
    return mpack;
}
// 释放预处理语句 mpack_field 数组各元素持有的列定义包 payload（由 _mpack_parse_field 转移而来；
// 结果集的在 reader 块链里，不走这里）。数组本身由调用方 FREE。CALLOC 保证未解析槽位的 payload 为 NULL，FREE 对 NULL 安全
static void _mpack_fields_free(mpack_field *fields, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        FREE(fields[i].payload);
    }
}
// 列定义包、行包与行数组都在 reader 的块链里，整条链一起还
void _mpack_reader_free(void *pack) {
    mysql_reader_ctx *reader = pack;
    mem_arena_free(&reader->arena);
    mrow_arr_free(&reader->arr_rows);
}
// 初始化结果集读取器并挂载到 mysql->mpack，准备接收列字段描述。
// 列数先读出来，列描述数组与 reader 同一次分配(紧跟在结构体后面)，随 reader 释放
static int32_t _mpack_reader_new(mysql_ctx *mysql, binary_ctx *breader, mpack_type pktype) {
    int32_t _rtn;
    uint64_t fc = _mysql_get_lenenc(breader, &_rtn);
    if (ERR_OK != _rtn || fc > UINT16_MAX) {
        return ERR_FAILED;
    }
    mysql->mpack = _mpack_new();
    mysql_reader_ctx *reader;
    CALLOC(reader, 1, sizeof(mysql_reader_ctx) + sizeof(mpack_field) * (size_t)fc);
    reader->pack_type = pktype;
    reader->field_count = (int32_t)fc;
    if (reader->field_count > 0) {
        reader->fields = (mpack_field *)(reader + 1);
    }
    mrow_arr_init(&reader->arr_rows, 64);
    mysql->mpack->pack = reader;
    mysql->mpack->_free_mpack = _mpack_reader_free;
    mysql->mpack->pack_type = pktype;
    return ERR_OK;
}
// 从缓冲区继续读取下一个 MySQL 数据包，并初始化 breader 指向新 payload
static inline int32_t _mpack_more_data(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    size_t payload_lens;
    char *payload = _mysql_payload(mysql, buf, &payload_lens, status);
    if (NULL == payload) {
        return ERR_FAILED;
    }
    binary_init_read(breader, payload, payload_lens);
    return ERR_OK;
}
// 检查 EOF 包中的状态标志。进来时调用方已保证至少剩 1 字节（判过 offset < size 且 peek 过）。
// 协商了 CLIENT_DEPRECATE_EOF 时终止包按 OK 包读，只取 status_flags 与 session track，
// 计数不落 ctx：同老式 EOF，结果集不改 affected_rows / last_id
static inline eof_final _mpack_check_final(mysql_ctx *mysql, binary_ctx *breader, int32_t *status) {
    binary_get_skip(breader, 1);
    int16_t status_flags;
    int32_t rtn = BIT_CHECK(mysql->client.caps, CLIENT_DEPRECATE_EOF)
        ? _mpack_ok_parse(mysql, breader, NULL, NULL, &status_flags)
        : _mpack_eof(breader, &status_flags);
    if (ERR_OK != rtn) {
        return EOF_FINAL_BROKEN;
    }
    if (BIT_CHECK(status_flags, SERVER_MORE_RESULTS_EXISTS)) {
        BIT_SET(*status, PROT_MOREDATA);
        return EOF_FINAL_MORE;
    }
    return EOF_FINAL_DONE;
}
// 取块尾的行数组，布局见 _mpack_arena。不清零：两个行循环每列都把三个字段写全
static inline mpack_row *_mpack_row_place(binary_ctx *breader) {
    return (mpack_row *)(breader->data - MYSQL_HEAD_LENS + ROUND_UP(MYSQL_HEAD_LENS + breader->size, 8));
}
// 同 _mysql_lenenc，但读位置在调用方的局部量 *off 里：行数组的写与 breader 成员同型，
// 放在 breader 里每列都要回写再重读。只有走 _mysql_get_lenenc 的慢路径才回写 breader->offset
static inline uint64_t _mpack_row_lenenc(binary_ctx *breader, const char *data, size_t size,
                                         size_t *off, int32_t *err) {
    uint64_t v;
    if (*off < size && (uint8_t)data[*off] <= 0xfa) {
        *err = ERR_OK;
        return (uint8_t)data[(*off)++];
    }
    breader->offset = *off;
    v = _mysql_get_lenenc(breader, err);
    *off = breader->offset;
    return v;
}
// 解析文本协议（COM_QUERY）结果集中的一行数据，字段值以 lenenc 字符串存储
static int32_t _mpack_parse_text_row(mysql_reader_ctx *reader, binary_ctx *breader) {
    int32_t _rtn;
    uint64_t vlens;
    const int32_t nf = reader->field_count;
    char *data = breader->data;
    const size_t size = breader->size;
    size_t off = breader->offset;
    mpack_row *row = _mpack_row_place(breader);
    for (int32_t i = 0; i < nf; i++) {
        if (off >= size) {
            return ERR_FAILED;
        }
        if (0xfb == (uint8_t)data[off]) {
            row[i].nil = 1; // 0xfb 表示 NULL 值
            row[i].val.lens = 0;
            row[i].val.data = NULL;
            off++;
            continue;
        }
        // 比过再收窄，不能反过来：转 size_t 是有损的，32 位构建上截断后的值能骗过判定
        vlens = _mpack_row_lenenc(breader, data, size, &off, &_rtn);
        if (ERR_OK != _rtn
            || vlens > (uint64_t)(size - off)) {
            return ERR_FAILED;
        }
        row[i].nil = 0;
        row[i].val.lens = (size_t)vlens;
        row[i].val.data = (0 == vlens) ? NULL : data + off;
        off += (size_t)vlens;
    }
    breader->offset = off;
    mrow_arr_push_back(&reader->arr_rows, &row);
    return ERR_OK;
}
// 解析二进制协议（COM_STMT_EXECUTE）结果集中的一行数据，字段值按类型固定或 lenenc 长度读取
static int32_t _mpack_parse_binary_row(mysql_reader_ctx *reader, binary_ctx *breader) {
    int32_t bit;
    int32_t _rtn;
    uint64_t vlens;
    size_t lens;
    const int32_t nf = reader->field_count;
    const mpack_field *fields = reader->fields;
    char *data = breader->data;
    const size_t size = breader->size;
    size_t off = breader->offset;
    mpack_row *row = _mpack_row_place(breader);
    // 读取 NULL 位图（偏移量 +2 是因为二进制协议位图从第 3 位开始）。
    // 位图长度来自上一个包声明的 field_count（上限 65535，位图可达 8193 字节），
    // 与本包实际长度无关，故读之前必须比一遍
    size_t bmlens = ((size_t)nf + 9) / 8;
    if (bmlens > size - off) {
        return ERR_FAILED;
    }
    const char *bitmap = data + off;
    off += bmlens;
    for (int32_t i = 0; i < nf; i++) {
        bit = i + 2;
        if (BIT_CHECK(bitmap[(bit / 8)], (1 << (bit % 8)))) {
            row[i].nil = 1;
            row[i].val.lens = 0;
            row[i].val.data = NULL;
            continue;
        }
        switch (fields[i].type) {
        case MYSQL_TYPE_LONGLONG:
            lens = sizeof(int64_t);
            break;
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:
            lens = sizeof(int32_t);
            break;
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_YEAR:
            lens = sizeof(int16_t);
            break;
        case MYSQL_TYPE_TINY:
            lens = sizeof(int8_t);
            break;
        case MYSQL_TYPE_DOUBLE:
            lens = sizeof(double);
            break;
        case MYSQL_TYPE_FLOAT:
            lens = sizeof(float);
            break;
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_DATETIME2:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2:
            if (off >= size) {// 长度前缀本身也可能被截断
                return ERR_FAILED;
            }
            lens = (uint8_t)data[off++];
            if (0 != lens && 4 != lens
                && 7 != lens && 11 != lens) {
                return ERR_FAILED;
            }
            break;
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_TIME2:
            if (off >= size) {// 长度前缀本身也可能被截断
                return ERR_FAILED;
            }
            lens = (uint8_t)data[off++];
            if (0 != lens && 8 != lens && 12 != lens) {
                return ERR_FAILED;
            }
            break;
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_ENUM:
        case MYSQL_TYPE_SET:
        case MYSQL_TYPE_LONG_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_GEOMETRY:
        case MYSQL_TYPE_BIT:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
        case MYSQL_TYPE_JSON:
            // 字符串/BLOB 类型以 lenenc 长度编码。比过再收窄，理由同 _mpack_parse_text_row
            vlens = _mpack_row_lenenc(breader, data, size, &off, &_rtn);
            if (ERR_OK != _rtn
                || vlens > (uint64_t)(size - off)) {
                return ERR_FAILED;
            }
            lens = (size_t)vlens;
            break;
        default:
            LOG_WARN("unknow data type %d.", (int32_t)fields[i].type);
            return ERR_FAILED;
        }
        // 定长分支的长度虽是常量，截断的报文照样读不出来，故与 lenenc 分支共用这道判定
        if (lens > size - off) {
            return ERR_FAILED;
        }
        row[i].nil = 0;
        row[i].val.lens = lens;
        row[i].val.data = (0 == lens) ? NULL : data + off;
        off += lens;
    }
    breader->offset = off;
    mrow_arr_push_back(&reader->arr_rows, &row);
    return ERR_OK;
}
// 结果集字段/行阶段遇 ERR(0xff) 包：MySQL 协议允许 ERR 提前终止结果集(KILL QUERY / max_execution_time 等)，
// 须解析错误并以 MPACK_ERR 完成本 mpack 而非当协议错误断连；释放已积累的 reader，返回不带 payload 的错误结果。
// 本包在 reader 的块链里，必须先由 _mpack_err 把错误码与错误串拷进 mysql，再释放 reader
static mpack_ctx *_mpack_reader_err(mysql_ctx *mysql, binary_ctx *breader) {
    binary_get_skip(breader, 1);
    _mpack_err(mysql, breader);
    _mysql_pkfree(mysql->mpack);
    mysql->mpack = NULL;
    mpack_ctx *mpack = _mpack_new();
    mpack->pack_type = MPACK_ERR;
    mysql->parse_status = 0;
    mysql->cur_cmd = 0;
    return mpack;
}
// 循环读取并解析结果集行数据，直到遇到 EOF 包结束。这一阶段的包都在 reader 的块链里(见 _mpack_arena)，
// 不单独释放；例外是没有列时包是单独分配的
static mpack_ctx *_mpack_reader_rows(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    uint8_t first;
    eof_final fin;
    mpack_ctx *mpack;
    mysql_reader_ctx *reader = mysql->mpack->pack;
    if (reader->field_count <= 0) {
        BIT_SET(*status, PROT_ERROR);
        FREE(breader->data);
        return NULL;
    }
    for (;;) {
        if (!binary_have(breader, 1)) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        first = _mysql_peek(breader);
        if (MYSQL_ERR == first) {
            return _mpack_reader_err(mysql, breader);
        }
        if (_mysql_is_eof_packet(mysql, breader)) {
            fin = _mpack_check_final(mysql, breader, status);
            if (EOF_FINAL_BROKEN == fin) {
                BIT_SET(*status, PROT_ERROR);
                return NULL;
            }
            if (EOF_FINAL_MORE == fin) {
                BIT_REMOVE(*status, PROT_MOREDATA);
                mpack = mysql->mpack;
                mpack->more = 1;
                mysql->mpack = NULL;
                mysql->parse_status = 0;
                return mpack;
            }
            mpack = mysql->mpack; // 行解析完成，返回完整结果集
            mysql->mpack = NULL;
            mysql->cur_cmd = 0;
            mysql->parse_status = 0;
            return mpack;
        }
        if (MPACK_QUERY == mysql->mpack->pack_type) {
            if (ERR_OK != _mpack_parse_text_row(reader, breader)) {
                BIT_SET(*status, PROT_ERROR);
                return NULL;
            }
        } else {
            if (0x00 != first) {
                BIT_SET(*status, PROT_ERROR);
                return NULL;
            }
            binary_get_skip(breader, 1);
            if (ERR_OK != _mpack_parse_binary_row(reader, breader)) {
                BIT_SET(*status, PROT_ERROR);
                return NULL;
            }
        }
        if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
            return NULL;
        }
    }
    return NULL;
}
// 读一个 lenenc 字符串：buf->data 指向 payload 内的原始字节（非 NUL 结尾），lens 为 0 时 data 为 NULL；
// lenenc 本身读取失败返回 ERR_FAILED
static inline int32_t _mpack_parse_lenenc_field(binary_ctx *breader, buf_ctx *buf) {
    int32_t rtn;
    uint64_t lens = _mysql_lenenc(breader, &rtn);
    if (ERR_OK != rtn
        || !binary_have(breader, lens)) {
        return ERR_FAILED;
    }
    buf->lens = (size_t)lens;
    buf->data = binary_get_binary(breader, buf->lens);
    return ERR_OK;
}
// 解析单个列字段描述包（Column Definition），填充 mpack_field 结构体。
// 返 ERR_OK 时 field->payload 记下 breader->data（5 个名字的 buf_ctx 指向其中）：结果集的包在 reader 块链里、
// 随 reader 释放；预处理语句的包所有权转给 field，调用方不得再 FREE。返 ERR_FAILED 时不记，包仍归调用方
static int32_t _mpack_parse_field(binary_ctx *breader, mpack_field *field) {
    int32_t _rtn;
    uint64_t lens = _mysql_lenenc(breader, &_rtn);
    if (ERR_OK != _rtn
        || !binary_have(breader, lens)) {
        return ERR_FAILED;
    }
    binary_get_skip(breader, (size_t)lens);//catalog（跳过 catalog 字段）
    if (ERR_OK != _mpack_parse_lenenc_field(breader, &field->schema)) {
        return ERR_FAILED;
    }
    if (ERR_OK != _mpack_parse_lenenc_field(breader, &field->table)) {
        return ERR_FAILED;
    }
    if (ERR_OK != _mpack_parse_lenenc_field(breader, &field->org_table)) {
        return ERR_FAILED;
    }
    if (ERR_OK != _mpack_parse_lenenc_field(breader, &field->name)) {
        return ERR_FAILED;
    }
    if (ERR_OK != _mpack_parse_lenenc_field(breader, &field->org_name)) {
        return ERR_FAILED;
    }
    _mysql_lenenc(breader, &_rtn);//length of fixed length fields（跳过固定长度字段的长度标志）
    if (ERR_OK != _rtn) {
        return ERR_FAILED;
    }
    // 尾部 10 字节定宽字段：character(2) field_lens(4) type(1) flags(2) decimals(1)
    if (!binary_have(breader, 10)) {
        return ERR_FAILED;
    }
    field->character = (int16_t)binary_get_integer(breader, 2, 1);
    field->field_lens = (int32_t)binary_get_integer(breader, 4, 1);
    field->type = binary_get_uint8(breader);
    field->flags = (uint16_t)binary_get_uinteger(breader, 2, 1);
    field->decimals = binary_get_uint8(breader);
    field->payload = breader->data;
    return ERR_OK;
}
// 循环读取并解析结果集列字段描述，字段解析完毕后继续解析行数据。
// 协商了 CLIENT_DEPRECATE_EOF 时列定义后没有 EOF，收满 field_count 个即转入行阶段
static mpack_ctx *_mpack_reader_fileds(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    uint8_t first;
    mysql_reader_ctx *reader = mysql->mpack->pack;
    for (;;) {
        if (!binary_have(breader, 1)) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        first = _mysql_peek(breader);
        if (MYSQL_ERR == first) {
            return _mpack_reader_err(mysql, breader);
        }
        if (_mysql_is_eof_packet(mysql, breader)) {
            // 字段阶段 EOF 仅标记列定义结束，无条件转入行阶段：SERVER_MORE_RESULTS 是结果集级状态，
            // 须在行阶段 EOF 判定；多语句 / CALL 时字段 EOF 同样带 more 位，此处若据 more 报错会误判断连
            if (reader->index != reader->field_count) {
                // 列定义比声明的列数少：没收到的那几列还是 CALLOC 的全零，而 type 0 恰好是合法枚举
                // MYSQL_TYPE_DECIMAL、name 长度为 0，行解析会照着这份假元数据把整行拆错位
                BIT_SET(*status, PROT_ERROR);
                return NULL;
            }
            reader->index = 0;
            mysql->parse_status = RST_ROW; // 字段解析完成，切换到行解析阶段
            break;
        }
        if (reader->index >= reader->field_count) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        if (ERR_OK != _mpack_parse_field(breader, &reader->fields[reader->index])) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        ++reader->index;
        if (reader->index == reader->field_count
            && BIT_CHECK(mysql->client.caps, CLIENT_DEPRECATE_EOF)) {
            reader->index = 0;
            mysql->parse_status = RST_ROW;
            break;
        }
        if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
            return NULL;
        }
    }
    if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
        return NULL;
    }
    return _mpack_reader_rows(mysql, buf, breader, status);
}
// 根据当前解析状态（字段阶段/行阶段）分发到对应处理函数
static mpack_ctx *_mpack_reader(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = NULL;
    switch (mysql->parse_status) {
    case RST_FIELD:
        mpack = _mpack_reader_fileds(mysql, buf, breader, status);
        break;
    case RST_ROW:
        mpack = _mpack_reader_rows(mysql, buf, breader, status);
        break;
    default:
        break;
    }
    return mpack;
}
// 解析 COM_QUERY / COM_STMT_EXECUTE 响应：可能是 OK/ERR 或带字段+行数据的结果集；
// restype 传 MPACK_QUERY 或 MPACK_STMT_EXECUTE 决定结果集读取器的协议解读方式；
// LOCAL INFILE 首字节仅 COM_QUERY 文本协议会出现，STMT_EXECUTE 二进制协议不适用该分支
static mpack_ctx *_mpack_resultset_response(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status, mpack_type restype) {
    mpack_ctx *mpack = NULL;
    if (0 == mysql->parse_status) {
        uint8_t first = (uint8_t)(binary_at(breader, 0)[0]);
        if (MYSQL_OK == first) {
            binary_get_skip(breader, 1);
            mpack = _mpack_new();
            mpack->pack_type = MPACK_OK;
            int16_t status_flags;
            if (ERR_OK != _mpack_ok(mysql, breader, &status_flags)) {
                BIT_SET(*status, PROT_ERROR);
                _mysql_pkfree(mpack);
                return NULL;
            }
            // 与行阶段 EOF 续接同规则(见 _mpack_check_final)：OK 包若声明还有更多结果集(多语句/CALL
            // 多结果集)，保留 cur_cmd 供下一个包续接解析；否则下一响应会落入 default 分支误判协议错误断连
            if (BIT_CHECK(status_flags, SERVER_MORE_RESULTS_EXISTS)) {
                mpack->more = 1;
            } else {
                mysql->cur_cmd = 0;
            }
        } else if (MYSQL_ERR == first) {
            binary_get_skip(breader, 1);
            mpack = _mpack_new();
            mpack->pack_type = MPACK_ERR;
            _mpack_err(mysql, breader);
            mysql->cur_cmd = 0;
        } else if (MPACK_QUERY == restype && MYSQL_LOCAL_INFILE == first) {
            // 不支持 LOCAL INFILE，直接报错
            BIT_SET(*status, PROT_ERROR);
        } else {
            // 结果集响应：先读取列数
            if (ERR_OK != _mpack_reader_new(mysql, breader, restype)) {
                BIT_SET(*status, PROT_ERROR);
            } else {
                mysql->parse_status = RST_FIELD;
                if (ERR_OK == _mpack_more_data(mysql, buf, breader, status)) {
                    mpack = _mpack_reader(mysql, buf, breader, status);
                }
            }
        }
    } else {
        // 续接解析（数据分片场景）
        mpack = _mpack_reader(mysql, buf, breader, status);
    }
    return mpack;
}
// 循环读取并解析 STMT_PREPARE 响应中的参数字段和结果集字段描述。每段以 EOF 收尾；
// 协商了 CLIENT_DEPRECATE_EOF 时没有 EOF，收满本段声明的条数即算收完(此时不能再去等下一个包)
static mpack_ctx *_mpack_stmt(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack;
    int32_t declared;
    mpack_field *defs;
    mysql_stmt_ctx *stmt = mysql->mpack->pack;
    for (;;) {
        // 与 _mpack_reader_fileds / _mpack_reader_rows 风格一致：恶意/受损服务端发空
        // packet (payload_lens=0) 时 breader->size=0，binary_at 触发 ASSERTAB → abort
        if (!binary_have(breader, 1)) {
            BIT_SET(*status, PROT_ERROR);
            FREE(breader->data);
            return NULL;
        }
        if (STMT_PREPARE_PARAMS == mysql->parse_status) {
            declared = (int32_t)stmt->params_count;
            defs = stmt->params;
        } else {
            declared = (int32_t)stmt->field_count;
            defs = stmt->fields;
        }
        if (_mysql_is_eof_packet(mysql, breader)) {
            // 本阶段不允许续接结果集，EOF_FINAL_MORE 与截断一样按协议错误处理
            if (EOF_FINAL_DONE != _mpack_check_final(mysql, breader, status)) {
                BIT_SET(*status, PROT_ERROR);
                FREE(breader->data);
                return NULL;
            }
            // 同 _mpack_reader_fileds：本阶段收到的定义条数必须正好等于声明数，
            // 少了就会留下全零的假元数据被后续按合法字段用
            if (stmt->index != declared) {
                BIT_SET(*status, PROT_ERROR);
                FREE(breader->data);
                return NULL;
            }
            FREE(breader->data);
        } else {
            if (stmt->index >= declared) {
                BIT_SET(*status, PROT_ERROR);
                FREE(breader->data);
                return NULL;
            }
            if (ERR_OK != _mpack_parse_field(breader, &defs[stmt->index])) {
                BIT_SET(*status, PROT_ERROR);
                FREE(breader->data);
                return NULL;
            }
            ++stmt->index;
            if (stmt->index != declared
                || !BIT_CHECK(mysql->client.caps, CLIENT_DEPRECATE_EOF)) {
                if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
                    return NULL;
                }
                continue;
            }
        }
        if (STMT_PREPARE_PARAMS == mysql->parse_status
            && stmt->field_count > 0) {
            // 参数字段解析完成，切换到结果集字段解析阶段
            mysql->parse_status = STMT_PREPARE_FIELD;
            stmt->index = 0;
            if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
                return NULL;
            }
            continue;
        }
        mpack = mysql->mpack;
        mysql->mpack = NULL;
        mysql->cur_cmd = 0;
        mysql->parse_status = 0;
        return mpack;
    }
    return NULL;
}
mysql_stmt_ctx *mysql_stmt_init(mpack_ctx *mpack) {
    if (NULL == mpack
        || NULL == mpack->pack
        || MPACK_STMT_PREPARE != mpack->pack_type) {
        return NULL;
    }
    mysql_stmt_ctx *stmt = mpack->pack;
    // 将所有权从 mpack 转移给调用方，避免重复释放
    mpack->pack = NULL;
    mpack->_free_mpack = NULL;
    return stmt;
}
void _mpack_stm_free(void *pack) {
    mysql_stmt_ctx *stmt = pack;
    _mpack_fields_free(stmt->params, (int32_t)stmt->params_count);
    FREE(stmt->params);
    _mpack_fields_free(stmt->fields, (int32_t)stmt->field_count);
    FREE(stmt->fields);
}
// 分配并初始化预处理语句上下文，从 STMT_PREPARE OK 响应包中读取 stmt_id、字段数和参数数。
// 包体装不下这三个字段时返回 ERR_FAILED，此前不分配任何东西
static int32_t _mpack_stmt_new(mysql_ctx *mysql, binary_ctx *breader) {
    if (!binary_have(breader, 8)) {// stmt_id(4) + field_count(2) + params_count(2)
        return ERR_FAILED;
    }
    mysql->mpack = _mpack_new();
    mysql->mpack->pack_type = MPACK_STMT_PREPARE;
    mysql_stmt_ctx *stmt;
    CALLOC(stmt, 1, sizeof(mysql_stmt_ctx));
    stmt->mysql = mysql;
    stmt->skid = mysql->client.sk.skid;
    stmt->stmt_id = (int32_t)binary_get_integer(breader, 4, 1);
    stmt->field_count = (uint16_t)binary_get_uinteger(breader, 2, 1);
    stmt->params_count = (uint16_t)binary_get_uinteger(breader, 2, 1);
    if (stmt->field_count > 0) {
        mysql->parse_status = STMT_PREPARE_FIELD;
        CALLOC(stmt->fields, 1, sizeof(mpack_field) * (size_t)stmt->field_count);
    }
    if (stmt->params_count > 0) {
        // 参数字段优先解析（覆盖字段阶段状态）
        mysql->parse_status = STMT_PREPARE_PARAMS;
        CALLOC(stmt->params, 1, sizeof(mpack_field) * (size_t)stmt->params_count);
    }
    mysql->mpack->pack = stmt;
    mysql->mpack->_free_mpack = _mpack_stm_free;
    return ERR_OK;
}
// 解析 COM_STMT_PREPARE 响应：ERR 直接返回，OK 后继续解析参数和字段描述
static mpack_ctx *_mpack_prepare_response(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    if (0 == mysql->parse_status) {
        if (MYSQL_ERR == (uint8_t)(binary_at(breader, 0)[0])) {
            binary_get_skip(breader, 1);
            mpack_ctx * mpack = _mpack_new();
            mpack->pack_type = MPACK_ERR;
            _mpack_err(mysql, breader);
            mysql->cur_cmd = 0;
            return mpack;
        }
        binary_get_skip(breader, 1);
        int32_t rtn = _mpack_stmt_new(mysql, breader);
        if (ERR_OK != rtn) {
            LOG_ERROR("mysql stmt prepare response too short.");
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        if (0 == mysql->parse_status) {
            // 无参数无字段，立即返回
            mpack_ctx *mpack = mysql->mpack;
            mysql->mpack = NULL;
            mysql->cur_cmd = 0;
            return mpack;
        }
        if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
            return NULL;
        }
        return _mpack_stmt(mysql, buf, breader, status);
    } else {
        // 续接解析（数据分片场景）
        return _mpack_stmt(mysql, buf, breader, status);
    }
}
// 这里没有 MYSQL_QUIT 分支，也不该加：mysql_pack_quit 是唯一的 COM_QUIT 组包入口，而它
// 刻意不接 mysql_ctx、不写 cur_cmd —— 上层 handle 的析构路径会在工作线程调它，网络线程同时正拿
// id / cur_cmd 解析来包，写一下就是无同步的跨线程写（test_mysql_pack 有用例钉着这条）。
// 所以 cur_cmd 永远不会是 MYSQL_QUIT，写了也是死代码。
// 真实情况也用不上：服务端收到 COM_QUIT 直接断连不回包，走不到解析
mpack_ctx *_mpack_parser(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = NULL;
    switch (mysql->cur_cmd) {
    case MYSQL_INIT_DB:
        mpack = _mpack_simple_response(mysql, breader, status);
        if (NULL != mpack
            && MPACK_OK == mpack->pack_type
            && !EMPTYSTR(mysql->pending_db)) {
            // pending_db 与 client.database 等长，且 pending_db 在 mysql_pack_selectdb 已校验过，装得下
            safe_fill_str(mysql->client.database, sizeof(mysql->client.database), mysql->pending_db);
        }
        break;
    case MYSQL_PING:
    case MYSQL_STMT_RESET:
        mpack = _mpack_simple_response(mysql, breader, status);
        break;
    case MYSQL_QUERY:
        mpack = _mpack_resultset_response(mysql, buf, breader, status, MPACK_QUERY);
        break;
    case MYSQL_PREPARE:
        mpack = _mpack_prepare_response(mysql, buf, breader, status);
        break;
    case MYSQL_EXECUTE:
        mpack = _mpack_resultset_response(mysql, buf, breader, status, MPACK_STMT_EXECUTE);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    if (NULL == mpack && BIT_CHECK(*status, PROT_ERROR) && NULL != mysql->mpack) {
        _mysql_pkfree(mysql->mpack);
        mysql->mpack = NULL;
        mysql->parse_status = 0;
        mysql->cur_cmd = 0;
    }
    return mpack;
}
