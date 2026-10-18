#include "protocol/mysql/mysql.h"
#include "protocol/mysql/mysql_parse.h"
#include "protocol/mysql/mysql_utils.h"
#include "protocol/mysql/mysql_pack.h"
#include "protocol/prots_pub.h"
#include "mysql_bind.h"
#include "crypt/digest.h"
#include "event/event.h"

//https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html
// 客户端能力标志位组合：涵盖协议 4.1、多结果集、插件认证、连接属性等必要能力。
// 不带 CLIENT_INTERACTIVE：带了服务端按 interactive_timeout 而不是 wait_timeout 回收空闲连接
#define CLIENT_CAPS\
    (CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41 | CLIENT_RESERVED2 |\
    CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS | CLIENT_PS_MULTI_RESULTS | CLIENT_PLUGIN_AUTH | CLIENT_CONNECT_ATTRS |\
    CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA | CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS | CLIENT_QUERY_ATTRIBUTES | CLIENT_SESSION_TRACK | CLIENT_DEPRECATE_EOF)
#define MYSQL_CHARSET_UTF8MB4 45// utf8mb4_general_ci：5.7 与 MariaDB 的 utf8mb4 默认排序规则
#define MYSQL_CHARSET_UTF8MB4_0900 255// utf8mb4_0900_ai_ci：MySQL 8.0 起的 utf8mb4 默认排序规则

// 连接解析状态枚举
typedef enum parse_status {
    INIT = 0,       // 初始状态，等待服务器握手包
    SSL_EXCHANGE,   // SSL 握手交换中
    AUTH_PROCESS,   // 认证响应处理中
    COMMAND         // 命令交互阶段
}parse_status;
// 认证插件切换请求数据
typedef struct mpack_auth_switch {
    char *plugin;       // 切换目标认证插件名称
    buf_ctx provided;   // 插件提供的初始认证数据
}mpack_auth_switch;
// 连接属性键值对
typedef struct connect_attr {
    const char *key;
    const char *val;
}connect_attr;
// 字符集名称与协议 ID 的对照项
typedef struct mysql_charset {
    uint8_t id;
    const char *name;
}mysql_charset;

// 握手完成后的推送回调函数指针
static _handshaked_push _hs_push;
// 字符集名称 → MySQL 协议里的字符集 ID，取 5.7 / MariaDB 下各字符集的默认排序规则；
// utf8mb4 连 MySQL 8.0+ 时发出前换成 255，见 _mysql_wire_charset
static const mysql_charset _mysql_charsets[] = {
    { 1, "big5" }, { 3, "dec8" }, { 4, "cp850" }, { 6, "hp8" }, { 7, "koi8r" }, { 8, "latin1" }, { 9, "latin2" },
    { 10, "swe7" }, { 11, "ascii" }, { 12, "ujis" }, { 13, "sjis" }, { 16, "hebrew" }, { 18, "tis620" },
    { 19, "euckr" }, { 22, "koi8u" }, { 24, "gb2312" }, { 25, "greek" }, { 26, "cp1250" }, { 28, "gbk" },
    { 30, "latin5" }, { 32, "armscii8" }, { 33, "utf8" }, { 36, "cp866" }, { 37, "keybcs2" }, { 38, "macce" },
    { 39, "macroman" }, { 40, "cp852" }, { 41, "latin7" }, { MYSQL_CHARSET_UTF8MB4, "utf8mb4" }, { 51, "cp1251" },
    { 56, "utf16le" }, { 57, "cp1256" }, { 59, "cp1257" }, { 63, "binary" }, { 92, "geostd8" }, { 95, "cp932" },
    { 97, "eucjpms" }, { 248, "gb18030" }
};

void _mysql_init(void *hspush) {
    _hs_push = (_handshaked_push)hspush;
}
void _mysql_pkfree(void *pack) {
    if (NULL == pack) {
        return;
    }
    mpack_ctx *mpack = pack;
    if (NULL != mpack->_free_mpack) {
        mpack->_free_mpack(mpack->pack);
    }
    FREE(mpack->pack);
    FREE(mpack);
}
int32_t mysql_more(mpack_ctx *mpack) {
    return (NULL != mpack) ? mpack->more : 0;
}
void _mysql_udfree(ud_cxt *ud) {
    if (NULL == ud->context) {
        return;
    }
    mysql_ctx *mysql = ud->context;
    _mysql_pkfree(mysql->mpack);
    mysql->mpack = NULL;
    mysql->parse_status = 0;
    mysql->cur_cmd = 0;
    mysql->recvlens = 0;
    sock_mark_closed(&mysql->client.sk);// 网络线程上写，理由见 sock_mark_closed
    ud->context = NULL;
    PROT_REF_RELEASE(mysql);
}
// 将字符集名称转换为 MySQL 协议中的字符集 ID，名称不分大小写；没查到打警告并返回 0
static inline uint8_t _mysql_charset(const char *charset) {
    size_t lens = strlen(charset) + 1;// 连 '\0' 一起比，长短不同在短的那头就对不上
    for (size_t i = 0; i < ARRAY_SIZE(_mysql_charsets); i++) {
        if (0 == memcasecmp(charset, _mysql_charsets[i].name, lens)) {
            return _mysql_charsets[i].id;
        }
    }
    LOG_WARN("unknown mysql charset: %s, send 0 to server.", charset);
    return 0;
}
uint8_t _mysql_wire_charset(const mysql_ctx *mysql) {
    if (MYSQL_CHARSET_UTF8MB4 != mysql->client.charset
        || NULL != strstr(mysql->version, "MariaDB")) {
        return mysql->client.charset;
    }
    // 版本串开头的主版本号，如 "8.4.9"、"8.0.36-0ubuntu0.22.04.1"
    uint64_t major;
    if (ERR_OK != strtou64(mysql->version, strspn(mysql->version, "0123456789"), UINT64_MAX, &major)
        || major < 8) {
        return mysql->client.charset;
    }
    return MYSQL_CHARSET_UTF8MB4_0900;
}
// 使用 mysql_native_password 算法计算认证签名（SHA1 双重哈希后与盐值异或）
static void _mysql_native_sign(mysql_ctx *mysql, char sh1[SHA1_BLOCK_SIZE]) {
    char shpsw[SHA1_BLOCK_SIZE];
    char shscr[SHA1_BLOCK_SIZE];
    digest_ctx digest;
    digest_init(&digest, DG_SHA1);
    digest_update(&digest, mysql->client.password, strlen(mysql->client.password));
    digest_final(&digest, shpsw);
    digest_update(&digest, shpsw, SHA1_BLOCK_SIZE);
    digest_final(&digest, sh1);
    digest_update(&digest, mysql->server.salt, sizeof(mysql->server.salt));
    digest_update(&digest, sh1, SHA1_BLOCK_SIZE);
    digest_final(&digest, shscr);
    for (size_t i = 0; i < SHA1_BLOCK_SIZE; i++) {
        sh1[i] = shpsw[i] ^ shscr[i];
    }
    secure_zero(shpsw, sizeof(shpsw));
    secure_zero(shscr, sizeof(shscr));
    digest_free(&digest);
}
// 使用 caching_sha2_password 算法计算认证签名（SHA256 双重哈希后与盐值异或）
static void _mysql_caching_sha2_sign(mysql_ctx *mysql, char sh2[SHA256_BLOCK_SIZE]) {
    char shpsw[SHA256_BLOCK_SIZE];
    char shscr[SHA256_BLOCK_SIZE];
    digest_ctx digest;
    digest_init(&digest, DG_SHA256);
    digest_update(&digest, mysql->client.password, strlen(mysql->client.password));
    digest_final(&digest, shpsw);
    digest_update(&digest, shpsw, SHA256_BLOCK_SIZE);
    digest_final(&digest, sh2);
    digest_update(&digest, sh2, SHA256_BLOCK_SIZE);
    digest_update(&digest, mysql->server.salt, sizeof(mysql->server.salt));
    digest_final(&digest, shscr);
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
        sh2[i] = shpsw[i] ^ shscr[i];
    }
    secure_zero(shpsw, sizeof(shpsw));
    secure_zero(shscr, sizeof(shscr));
    digest_free(&digest);
}
// 将连接属性（application、os 等）连同前面的 lenenc 总长写入 bwriter；键值都是短常量(<251)，各自的 lenenc 恒为 1 字节
static void _mysql_connect_attrs(binary_ctx *bwriter) {
    static const connect_attr attrs[] = {
        { "application", "srey" },
        { "os", OS_NAME }
    };
    size_t total = 0;
    size_t i;
    for (i = 0; i < ARRAY_SIZE(attrs); i++) {
        total += 2 + strlen(attrs[i].key) + strlen(attrs[i].val);
    }
    _mysql_set_lenenc(bwriter, total);
    size_t lens;
    for (i = 0; i < ARRAY_SIZE(attrs); i++) {
        lens = strlen(attrs[i].key);
        _mysql_set_lenenc(bwriter, lens);
        binary_set_binary(bwriter, attrs[i].key, lens);
        lens = strlen(attrs[i].val);
        _mysql_set_lenenc(bwriter, lens);
        binary_set_binary(bwriter, attrs[i].val, lens);
    }
}
// 组包收尾：回填 3 字节长度头 → 失败即释放缓冲 → 成功把缓冲交给 ev_send(copy=0，所有权转移)。
// 认证阶段各处发包共用(前导见 _mysql_pack_begin)，别在调用点再各写一遍那个 binary_free
static inline int32_t _mysql_send_pack(mysql_ctx *mysql, ev_ctx *ev, binary_ctx *bwriter) {
    if (ERR_OK != _mysql_set_payload_lens(bwriter)) {
        binary_free(bwriter);
        return ERR_FAILED;
    }
    return ev_send(ev, &mysql->client.sk, bwriter->data, bwriter->offset, 0);
}
// 发送客户端认证响应包（HandshakeResponse），包含用户名、密码签名、连接属性等
static int32_t _mysql_auth_response(mysql_ctx *mysql, ev_ctx *ev, ud_cxt *ud) {
    binary_ctx bwriter;
    _mysql_pack_begin(mysql, &bwriter, 0, 0);
    binary_set_integer(&bwriter, mysql->client.caps, 4, 1);//client_flag
    binary_set_integer(&bwriter, mysql->client.maxpack, 4, 1);//max_packet_size
    binary_set_uint8(&bwriter, _mysql_wire_charset(mysql));//character_set
    binary_set_fill(&bwriter, 0, 23);//filler
    binary_set_string(&bwriter, mysql->client.user);//username
    if (EMPTYSTR(mysql->client.password)) {
        // 空密码：协议要求发送 0 长度 auth_response，服务端对空密码账户仅接受空响应
        if (BIT_CHECK(mysql->client.caps, CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)) {
            _mysql_set_lenenc(&bwriter, 0);
        } else {
            binary_set_uint8(&bwriter, 0);
        }
    } else if (0 == strcmp(CACHING_SHA2_PASSWORD, mysql->server.plugin)) {
        char sign[SHA256_BLOCK_SIZE];
        _mysql_caching_sha2_sign(mysql, sign);
        if (BIT_CHECK(mysql->client.caps, CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)) {
            _mysql_set_lenenc(&bwriter, sizeof(sign));
            binary_set_binary(&bwriter, sign, sizeof(sign));//auth_response
        } else {
            binary_set_uint8(&bwriter, (uint8_t)sizeof(sign));
            binary_set_binary(&bwriter, sign, sizeof(sign));//auth_response
        }
        secure_zero(sign, sizeof(sign));
    } else {
        char sign[SHA1_BLOCK_SIZE];
        _mysql_native_sign(mysql, sign);
        if (BIT_CHECK(mysql->client.caps, CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)) {
            _mysql_set_lenenc(&bwriter, sizeof(sign));
            binary_set_binary(&bwriter, sign, sizeof(sign));//auth_response
        } else {
            binary_set_uint8(&bwriter, (uint8_t)sizeof(sign));
            binary_set_binary(&bwriter, sign, sizeof(sign));//auth_response
        }
        secure_zero(sign, sizeof(sign));
    }
    if (BIT_CHECK(mysql->client.caps, CLIENT_CONNECT_WITH_DB)) {
        binary_set_string(&bwriter, mysql->client.database);//database
    }
    binary_set_string(&bwriter, mysql->server.plugin);//client_plugin_name；不带 CLIENT_PLUGIN_AUTH 的服务端握手时已拒收
    if (BIT_CHECK(mysql->client.caps, CLIENT_CONNECT_ATTRS)) {
        _mysql_connect_attrs(&bwriter);
    }
    ud->status = AUTH_PROCESS;
    return _mysql_send_pack(mysql, ev, &bwriter);
}
// 发送 SSL 握手请求包（SSLRequest），触发后续 SSL 升级
static int32_t _mysql_ssl_exchange(mysql_ctx *mysql, ev_ctx *ev, ud_cxt *ud) {
    binary_ctx bwriter;
    _mysql_pack_begin(mysql, &bwriter, 0, 0);
    binary_set_integer(&bwriter, mysql->client.caps, 4, 1);//client_flag
    binary_set_integer(&bwriter, mysql->client.maxpack, 4, 1);//max_packet_size
    binary_set_uint8(&bwriter, _mysql_wire_charset(mysql));//character_set
    binary_set_fill(&bwriter, 0, 23);//filler
    ud->status = SSL_EXCHANGE;
    if (ERR_OK != _mysql_send_pack(mysql, ev, &bwriter)
        || ERR_OK != ev_ssl(ev, &mysql->client.sk, 1, mysql->client.evssl)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t _mysql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud, void *ssl) {
    (void)ssl;// mysql 不做通道绑定，形参只为与 pgsql 同签名、共用 prots 的挂钩
    return _mysql_auth_response(ud->context, ev, ud);
}
// 认证失败：把 ERR 包体(error_code + [#sql_state] + 错误串)原样交给等待者，所有权随之交出；
// 连接级错误只在持串行化执行权的一侧写，由它经 mysql_record_handshake 落到连接上
static void _mysql_auth_err(mysql_ctx *mysql, ud_cxt *ud, binary_ctx *breader, int32_t *status) {
    BIT_SET(*status, PROT_ERROR);
    size_t lens = binary_remain(breader);
    char *body = 0 == lens ? NULL : dup_zero(binary_at(breader, breader->offset), lens);
    _hs_push(&mysql->client.sk, 1, ud, ERR_FAILED, body, lens);
}
// 认证成功：通知上层握手完成，切换到命令阶段
static void _mysql_auth_ok(mysql_ctx *mysql, ud_cxt *ud, int32_t *status) {
    if (ERR_OK != _hs_push(&mysql->client.sk, 1, ud, ERR_OK, NULL, 0)) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    ud->status = COMMAND;
}
// 解析初始握手包（Initial Handshake Packet），记下版本、盐值、能力标志与认证插件，定好本端能力位。
// 成功返回 NULL，失败返回日志文案。包长由对端决定，每段读之前都先比剩余字节：binary_get_* 越界会 ASSERTAB abort 整个进程
static const char *_mysql_handshake_parse(mysql_ctx *mysql, binary_ctx *breader) {
    if (0x0a != binary_get_uint8(breader)) {//protocol version
        return "mysql protocol version not 0x0a.";
    }
    char *val = binary_try_get_string(breader);//server version
    if (NULL == val) {
        return "mysql handshake truncated at server version.";
    }
    if (ERR_OK != safe_fill_str(mysql->version, sizeof(mysql->version), val)) {
        return "server version string too long.";
    }
    // thread_id(4) salt1(8) filler(1) caps1(2) charset(1) status(2) caps2(2)
    if (!binary_have(breader, 20)) {
        return "mysql handshake truncated at capability flags.";
    }
    binary_get_skip(breader, 4);//thread id
    val = binary_get_binary(breader, 8);//auth-plugin-data-part-1
    memcpy(mysql->server.salt, val, 8);
    binary_get_skip(breader, 1);//filler
    mysql->server.caps = (uint32_t)binary_get_uinteger(breader, 2, 1);//capability_flags_1
    binary_get_skip(breader, 1);
    binary_get_skip(breader, 2);//status_flags 无人消费，只推进读位置；要用换回下面这行
    //mysql->server.status_flags = (uint16_t)binary_get_uinteger(breader, 2, 1);
    mysql->server.caps |= ((uint32_t)binary_get_uinteger(breader, 2, 1) << 16);//capability_flags_2
    if (!BIT_CHECK(mysql->server.caps, CLIENT_PROTOCOL_41)
        || !BIT_CHECK(mysql->server.caps, CLIENT_PLUGIN_AUTH)) {
        return "CLIENT_PROTOCOL_41 or CLIENT_PLUGIN_AUTH is required.";
    }
    // auth_plugin_data_len(1) reserved(10) salt2(13)
    if (!binary_have(breader, 24)) {
        return "mysql handshake truncated at auth plugin data.";
    }
    if ((size_t)binary_get_uint8(breader) != sizeof(mysql->server.salt) + 1) {//auth_plugin_data_len
        return "auth_plugin_data_len error.";
    }
    binary_get_skip(breader, 10);//reserved
    val = binary_get_binary(breader, 13);//auth-plugin-data-part-2
    memcpy(mysql->server.salt + 8, val, 12);
    val = binary_try_get_string(breader);//auth_plugin_name
    if (NULL == val) {
        return "mysql handshake truncated at auth plugin name.";
    }
    // 服务端默认插件不认识(sha256_password、client_ed25519 等)就按 caching_sha2_password 作答，
    // 账号用的插件不同时服务端会发 AuthSwitch，切换目标仍不认识再失败
    safe_fill_str(mysql->server.plugin, sizeof(mysql->server.plugin),
                  0 == strcmp(val, MYSQL_NATIVE_PASSWORD) ? MYSQL_NATIVE_PASSWORD : CACHING_SHA2_PASSWORD);
    mysql->client.caps = CLIENT_CAPS & mysql->server.caps;
    if (0 != strlen(mysql->client.database)) {
        BIT_SET(mysql->client.caps, CLIENT_CONNECT_WITH_DB);
    }
    // 配置 evssl 即视为强制 SSL；服务端不广播 CLIENT_SSL 时必须拒收，
    // 杜绝 MITM 在明文 Initial Handshake Packet 中清除 CLIENT_SSL 位强制降级到明文认证
    if (NULL != mysql->client.evssl) {
        if (!BIT_CHECK(mysql->server.caps, CLIENT_SSL)) {
            return "server does not advertise CLIENT_SSL but client requires SSL.";
        }
        BIT_SET(mysql->client.caps, CLIENT_SSL);
    }
    return NULL;
}
// 收服务器初始握手包，解析通过后发 SSLRequest（配了 SSL）或认证响应
static void _mysql_auth_request(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_ctx *mysql = ud->context;
    char *payload = _mysql_payload(mysql, buf, &payload_lens, status);
    if (NULL == payload) {
        return;
    }
    binary_ctx breader;
    binary_init_read(&breader, payload, payload_lens);
    // 服务端可以不发握手包而直接回 ERR：1040 Too many connections、1129 Host is blocked、
    // 1130 Host is not privileged
    if (MYSQL_ERR == (uint8_t)payload[0]) {
        binary_get_skip(&breader, 1);
        _mysql_auth_err(mysql, ud, &breader, status);
        FREE(payload);
        return;
    }
    const char *err = _mysql_handshake_parse(mysql, &breader);
    FREE(payload);// 用到的字段解析时都已拷进 mysql
    if (NULL != err) {
        BIT_SET(*status, PROT_ERROR);
        LOG_ERROR("%s", err);
        return;
    }
    int32_t rtn = BIT_CHECK(mysql->client.caps, CLIENT_SSL)
        ? _mysql_ssl_exchange(mysql, ev, ud) : _mysql_auth_response(mysql, ev, ud);
    if (ERR_OK != rtn) {
        BIT_SET(*status, PROT_ERROR);
    }
}
// 在 SSL 已建立的情况下，直接明文发送密码给服务器（caching_sha2 完整认证 SSL 路径）
static int32_t _mysql_password_send(mysql_ctx *mysql, ev_ctx *ev) {
    binary_ctx bwriter;
    _mysql_pack_begin(mysql, &bwriter, 0, 0);
    binary_set_string(&bwriter, mysql->client.password);
    return _mysql_send_pack(mysql, ev, &bwriter);
}
// 处理服务器发起的认证插件切换请求，重新计算签名并发送响应
static int32_t _mysql_auth_switch_response(mysql_ctx *mysql, ev_ctx *ev, mpack_auth_switch *auswitch) {
    if (auswitch->provided.lens < sizeof(mysql->server.salt)
        || (0 != strcmp(auswitch->plugin, CACHING_SHA2_PASSWORD)
            && 0 != strcmp(auswitch->plugin, MYSQL_NATIVE_PASSWORD))) {
        return ERR_FAILED;
    }
    safe_fill_str(mysql->server.plugin, sizeof(mysql->server.plugin), auswitch->plugin);
    memcpy(mysql->server.salt, auswitch->provided.data, sizeof(mysql->server.salt));
    binary_ctx bwriter;
    _mysql_pack_begin(mysql, &bwriter, 0, 0);
    if (EMPTYSTR(mysql->client.password)) {
        // 空密码：AuthSwitchResponse 发送空 auth data，不写签名
    } else if (0 == strcmp(CACHING_SHA2_PASSWORD, mysql->server.plugin)) {
        char sign[SHA256_BLOCK_SIZE];
        _mysql_caching_sha2_sign(mysql, sign);
        binary_set_binary(&bwriter, sign, sizeof(sign));
        secure_zero(sign, sizeof(sign));
    } else {
        char sign[SHA1_BLOCK_SIZE];
        _mysql_native_sign(mysql, sign);
        binary_set_binary(&bwriter, sign, sizeof(sign));
        secure_zero(sign, sizeof(sign));
    }
    return _mysql_send_pack(mysql, ev, &bwriter);
}
// 处理服务器发来的认证插件切换包（Auth Switch Request）。CLIENT_PLUGIN_AUTH 握手时已确保协商上
static void _mysql_auth_switch(mysql_ctx *mysql, ev_ctx *ev, binary_ctx *breader, int32_t *status) {
    mpack_auth_switch auswitch;
    // 插件名的长度由对端决定，取不到 NUL 就判失败而不是撞 binary_get_string 的断言
    auswitch.plugin = binary_try_get_string(breader);
    if (NULL == auswitch.plugin) {
        BIT_SET(*status, PROT_ERROR);
        LOG_ERROR("mysql auth switch plugin name truncated.");
        return;
    }
    auswitch.provided.lens = binary_remain(breader);
    if (0 == auswitch.provided.lens) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    auswitch.provided.data = binary_get_binary(breader, auswitch.provided.lens);
    if (ERR_OK != _mysql_auth_switch_response(mysql, ev, &auswitch)) {
        BIT_SET(*status, PROT_ERROR);
        LOG_ERROR("mysql auth switch to %s failed.", auswitch.plugin);
    }
}
// 处理 caching_sha2_password 认证的中间响应：01 03 快速认证通过，01 04 要完整认证。
// 完整认证只在 TLS 上明文发口令；明文连接不做 RSA 公钥交换(公钥无从核实，中间人换掉它就能拿到明文口令)，直接判失败
static void _mysql_caching_sha2(mysql_ctx *mysql, ev_ctx *ev, binary_ctx *breader, int32_t *status) {
    if (2 != breader->size) {
        BIT_SET(*status, PROT_ERROR);
        LOG_ERROR("mysql caching_sha2 response size %zu, want 2.", breader->size);
        return;
    }
    uint8_t st = binary_get_uint8(breader);
    switch (st) {
    case MYSQL_CACHING_SHA2_FAST://fast auth
        break;
    case MYSQL_CACHING_SHA2_FULL://full auth
        if (!BIT_CHECK(mysql->client.caps, CLIENT_SSL)) {
            BIT_SET(*status, PROT_ERROR);
            LOG_ERROR("mysql caching_sha2_password full authentication needs SSL: enable SSL, log in once "
                      "over SSL so the server caches the password, or use a mysql_native_password account.");
            break;
        }
        if (ERR_OK != _mysql_password_send(mysql, ev)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        LOG_ERROR("mysql caching_sha2 unknown auth status %d.", (int32_t)st);
        break;
    }
}
// 认证阶段数据处理：根据服务器响应类型分发到 OK/ERR/认证切换/SHA2 处理函数
static void _mysql_auth_process(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_ctx *mysql = ud->context;
    char *payload = _mysql_payload(mysql, buf, &payload_lens, status);
    if (NULL == payload) {
        return;
    }
    binary_ctx breader;
    binary_init_read(&breader, payload, payload_lens);
    uint8_t first = binary_get_uint8(&breader);
    switch (first) {
    case MYSQL_OK:
        _mysql_auth_ok(mysql, ud, status);
        break;
    case MYSQL_ERR:
        _mysql_auth_err(mysql, ud, &breader, status);
        break;
    case MYSQL_AUTH_SWITCH:
        _mysql_auth_switch(mysql, ev, &breader, status);
        break;
    case MYSQL_CACHING_SHA2:
        _mysql_caching_sha2(mysql, ev, &breader, status);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        LOG_ERROR("mysql auth response unknown packet type %d.", (int32_t)first);
        break;
    }
    FREE(payload);
}
// 命令阶段数据处理：读取 payload 后交由 _mpack_parser 解析响应包。首包(parse_status 为 0)读进栈上缓冲，
// parser 用完不留(见 _mpack_parser)；续接包按各阶段的归属走
static mpack_ctx *_mysql_command_process(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    char *payload;
    binary_ctx breader;
    mysql_ctx *mysql = ud->context;
    if (0 != mysql->parse_status) {
        payload = _mysql_payload(mysql, buf, &payload_lens, status);
        if (NULL == payload) {
            return NULL;
        }
        binary_init_read(&breader, payload, payload_lens);
        return _mpack_parser(mysql, buf, &breader, status);
    }
    char stk[MYSQL_FIRST_STK];
    payload = _mysql_payload_first(mysql, buf, stk, sizeof(stk), &payload_lens, status);
    if (NULL == payload) {
        return NULL;
    }
    binary_init_read(&breader, payload, payload_lens);
    mpack_ctx *mpack = _mpack_parser(mysql, buf, &breader, status);
    if (payload != stk + MYSQL_HEAD_LENS) {
        FREE(payload);
    }
    return mpack;
}
void *mysql_unpack(ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    (void)sk; (void)client;
    if (NULL == ud->context) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    mysql_ctx *mysql = ud->context;
    size_t lens;
    mpack_ctx *pack = NULL;
    switch (ud->status) {
    case INIT:
        _mysql_auth_request(ev, buf, ud, status);
        break;
    case AUTH_PROCESS:
        _mysql_auth_process(ev, buf, ud, status);
        break;
    case COMMAND:
        // 一个响应可能跨多次收包才交出：已收字节攒在 recvlens，交出时整笔记给这个包
        lens = buffer_size(buf);
        pack = _mysql_command_process(buf, ud, status);
        mysql->recvlens += lens - buffer_size(buf);
        if (NULL != pack) {
            *size = mysql->recvlens;
            mysql->recvlens = 0;
        }
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    return pack;
}
int32_t mysql_init(mysql_ctx *mysql, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database, const char *charset, uint32_t maxpk) {
    ZERO(mysql, sizeof(mysql_ctx));
    // 字段装不下即失败返回，失败后的处置见 mysql.h 的 mysql_init
    if (ERR_OK != safe_fill_str(mysql->client.ip, sizeof(mysql->client.ip), ip)) {
        LOG_ERROR("mysql ip exceeds %zu bytes: %zu.", sizeof(mysql->client.ip) - 1, strlen(ip));
        return ERR_FAILED;
    }
    if (ERR_OK != safe_fill_str(mysql->client.user, sizeof(mysql->client.user), user)) {
        LOG_ERROR("mysql user name exceeds %zu bytes: %zu.", sizeof(mysql->client.user) - 1, strlen(user));
        return ERR_FAILED;
    }
    if (ERR_OK != safe_fill_str(mysql->client.password, sizeof(mysql->client.password), password)) {
        LOG_ERROR("mysql password exceeds %zu bytes: %zu.", sizeof(mysql->client.password) - 1, strlen(password));
        return ERR_FAILED;
    }
    if (ERR_OK != safe_fill_str(mysql->client.database, sizeof(mysql->client.database), database)) {
        LOG_ERROR("mysql database name exceeds %zu bytes: %zu.", sizeof(mysql->client.database) - 1, strlen(database));
        return ERR_FAILED;
    }
    mysql->client.port = 0 == port ? 3306 : port;
    mysql->client.evssl = evssl;
    mysql->client.charset = _mysql_charset(charset);
    mysql->client.maxpack = 0 == maxpk ? ONEK * ONEK : maxpk;
    sock_set_invalid(&mysql->client.sk);
    return ERR_OK;
}
const char *mysql_version(mysql_ctx *mysql) {
    return mysql->version;
}
const char *mysql_erro(mysql_ctx *mysql, int32_t *code) {
    SET_PTR(code, mysql->error_code);
    return mysql->error_msg;
}
void mysql_erro_clear(mysql_ctx *mysql) {
    mysql->error_code = ERR_OK;
    mysql->error_msg[0] = '\0';
}
uint64_t mysql_last_id(mysql_ctx *mysql) {
    return mysql->last_id;
}
uint64_t mysql_affected_rows(mysql_ctx *mysql) {
    return mysql->affected_rows;
}
uint64_t mysql_pack_affected_rows(mpack_ctx *mpack) {
    return mpack->affected_rows;
}
uint64_t mysql_pack_last_id(mpack_ctx *mpack) {
    return mpack->last_id;
}
const char *mysql_pack_erro(mpack_ctx *mpack, int32_t *code) {
    SET_PTR(code, mpack->error_code);
    // 别的类型的 pack 是 reader / stmt，不能当串读
    if (MPACK_ERR != mpack->pack_type
        || NULL == mpack->pack) {
        return "";
    }
    return mpack->pack;
}
const char *mysql_record_handshake(mysql_ctx *mysql, const char *body, size_t lens) {
    if (NULL == body
        || 0 == lens) {
        return "";
    }
    mpack_ctx err;
    ZERO(&err, sizeof(err));
    err.pack_type = MPACK_ERR;
    binary_ctx breader;
    binary_init_read(&breader, (char *)body, lens);
    _mpack_err(&err, &breader);
    mysql_record(mysql, &err);
    FREE(err.pack);
    return mysql->error_msg;
}
void mysql_record(mysql_ctx *mysql, mpack_ctx *mpack) {
    if (NULL == mpack) {
        return;
    }
    if (MPACK_OK == mpack->pack_type) {
        mysql->affected_rows = mpack->affected_rows;
        mysql->last_id = mpack->last_id;
    } else if (MPACK_ERR == mpack->pack_type) {
        const char *msg = mysql_pack_erro(mpack, NULL);
        mysql->error_code = mpack->error_code;
        // 错误串只给人看，装不下就截断
        (void)copy_bounded(msg, strlen(msg), mysql->error_msg, sizeof(mysql->error_msg), 0);
    }
}
