#include "lbind/lpub.h"

#define MT_SMTP      "_smtp_ctx"
#define MT_SMTP_MAIL "_smtp_mail_ctx"
// smtp check_codes 一次最多收几个码（栈上数组上限）。SMTP 单条命令的合法应答码就那么几个，
// 给到 8 已远超需要；超了直接报错而不是截断
#define SMTP_MAX_NCODE 8

//srey.smtp
/// <summary>
/// 创建 SMTP 客户端上下文（不立即建立连接）。只支持隐式 TLS、不支持 STARTTLS，见 C 层 smtp_init
/// </summary>
/// <param name="ip" type="string">SMTP 服务器 IP</param>
/// <param name="port" type="integer">SMTP 服务器端口</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文（明文下凭据也明文发出）</param>
/// <param name="user" type="string">认证用户名，最长 63 字节；空串为免认证投递：握手跳过 AUTH，服务端不通告 AUTH 也能连上</param>
/// <param name="psw" type="string">认证密码，最长 63 字节；user 为空串时不用</param>
/// <returns type="_smtp_ctx?">SMTP 对象；ip / user / psw 任一超长返回 nil
/// （同 mysql.new / pgsql.new / mongo.new，不静默截断）</returns>
static int32_t _lsmtp_new(lua_State *lua) {
    const char *ip = lpub_check_cstr(lua, 1);
    uint16_t port = lpub_check_u16(lua, 2, PORT_OUT_OF_RANGE);
    struct evssl_ctx *evssl = lpub_check_evssl(lua, 3);
    const char *user = lpub_check_cstr(lua, 4);
    const char *psw = lpub_check_cstr(lua, 5);
    smtp_ctx **ud = (smtp_ctx **)lpub_push_ud(lua, NULL, MT_SMTP);
    smtp_ctx *smtp;
    MALLOC(smtp, sizeof(smtp_ctx));
    if (ERR_OK != smtp_init(smtp, ip, port, evssl, user, psw)) {
        FREE(smtp);
        return lpub_rtn_nil(lua, 1);
    }
    ATOMIC_SET_RELAXED(&smtp->ref, 1);// Lua 持有者份额
    *ud = smtp;
    return 1;
}
/// <summary>
/// 发送 QUIT 命令并清理 SMTP 连接上下文（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns>无</returns>
static int32_t _lsmtp_free(lua_State *lua) {
    smtp_ctx **ud = lpub_check_udata(lua, 1, MT_SMTP);
    smtp_ctx *smtp = *ud;
    if (NULL == smtp) {
        return 0;
    }
    if (NULL != smtp->task && !sock_is_invalid(&smtp->sk)) {
        char *cmd = smtp_pack_quit();
        ev_send(&smtp->task->loader->netev, &smtp->sk, cmd, strlen(cmd), 0);
        // 主动关连接：触发该 socket 的 udfree 释放事件侧份额，否则弃用的活连接块滞留至对端关
        ev_close(&smtp->task->loader->netev, &smtp->sk);
    }
    *ud = NULL;
    // 密码不在这里擦：ev_close 只是投命令，网络线程可能正读着它组认证串。
    // 擦除已挪进 PROT_REF_RELEASE，由最后一个放手的线程整块抹掉
    PROT_REF_RELEASE(smtp);
    return 0;
}
/// <summary>
/// 返回 SMTP 连接的连接标识
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="userdata">连接标识；失败时其 valid 字段为 false</returns>
static int32_t _lsmtp_sock_id(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    smtp_ctx *smtp = *ud;
    lpub_push_sock_slot(lua, 1, &smtp->sk);
    return 1;
}
/// <summary>
/// 尝试建立 SMTP 连接（异步）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="boolean">发起成功 true，失败 false</returns>
static int32_t _lsmtp_try_connect(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    LPUB_CUR_TASK(lua, task);
    int32_t rtn = smtp_try_connect(task, *ud, 1);
    return lpub_rtn_bool(lua, ERR_OK == rtn);
}
/// <summary>
/// 检查 SMTP 响应包的状态码是否匹配指定 code，按前缀比较：传 "45" 命中 450~459
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="pack" type="lightuserdata">SMTP 响应包指针</param>
/// <param name="code" type="string">期望状态码（如 "220"、"250"）；空串报错（对任何应答都会命中）</param>
/// <returns type="boolean">匹配 true，否则 false</returns>
static int32_t _lsmtp_check_code(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    LPUB_LUD_ARG(lua, char, 2, pack);
    const char *code = lpub_check_cstr(lua, 3);
    luaL_argcheck(lua, '\0' != code[0], 3, "empty code");
    return lpub_rtn_bool(lua, ERR_OK == smtp_check_code(pack, code));
}
/// <summary>
/// 检查 SMTP 响应包的状态码是否命中 codes 里任意一个，逐个按前缀比较同 check_code。用于一条命令有多个合法应答的场合，
/// 如 RCPT TO 的 250(已接受) 与 251(已接受但将转发)。全不中才落一条告警
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="pack" type="lightuserdata">SMTP 响应包指针</param>
/// <param name="codes" type="string[]">期望状态码数组，如 { "250", "251" }；空表恒返 false，元素不是字符串或为空串报错</param>
/// <returns type="boolean">命中其中之一 true，否则 false</returns>
static int32_t _lsmtp_check_codes(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    LPUB_LUD_ARG(lua, char, 2, pack);
    luaL_checktype(lua, 3, LUA_TTABLE);
    lua_Integer ncode = (lua_Integer)lua_rawlen(lua, 3);
    luaL_argcheck(lua, ncode >= 0 && ncode <= SMTP_MAX_NCODE, 3, "too many codes");
    const char *codes[SMTP_MAX_NCODE];
    for (lua_Integer i = 0; i < ncode; i++) {
        lua_rawgeti(lua, 3, i + 1);
        // 必须已经是字符串:luaL_checkstring 会把数字就地转成串,而那种串只由栈槽锚定,
        // pop 之后可能被回收,codes[i] 就成了悬空指针。真字符串由表持有,pop 后仍有效
        luaL_argcheck(lua, LUA_TSTRING == lua_type(lua, -1) && 0 != lua_rawlen(lua, -1), 3, "codes must be non-empty strings");
        codes[i] = lua_tostring(lua, -1);
        lua_pop(lua, 1);
    }
    return lpub_rtn_bool(lua, ERR_OK == smtp_check_codes(pack, codes, (size_t)ncode));
}
/// <summary>
/// 检查 SMTP 响应包的应答码是否为 250。只认这一个码，不是判整个 2xx 段——
/// 251/252 这类同样表示"已接受"的应答会被判失败，要认它们得用 check_codes
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="pack" type="lightuserdata">SMTP 响应包指针</param>
/// <returns type="boolean">应答码为 250 返回 true，否则 false</returns>
static int32_t _lsmtp_check_ok(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    LPUB_LUD_ARG(lua, char, 2, pack);
    return lpub_rtn_bool(lua, ERR_OK == smtp_check_ok(pack));
}
/// <summary>
/// 构造 SMTP RSET 重置命令
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针，归调用方：srey.syn_send(copy=0) 交出或 ud_free</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lsmtp_pack_reset(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    char *cmd = smtp_pack_reset();
    return lpub_rtn_lud(lua, (void *)cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP MAIL FROM 命令；地址走 CRLF 注入防御
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="from" type="string">发件人邮箱地址</param>
/// <returns type="lightuserdata?">命令数据指针(释放同 pack_reset)；地址含 CRLF 时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lsmtp_pack_from(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    const char *from = lpub_check_cstr(lua, 2);
    char *cmd = smtp_pack_from(from);
    return lpub_rtn_lud(lua, cmd, NULL != cmd ? strlen(cmd) : 0);
}
/// <summary>
/// 构造 SMTP RCPT TO 命令；地址走 CRLF 注入防御
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="rcpt" type="string">收件人邮箱地址</param>
/// <returns type="lightuserdata?">命令数据指针(释放同 pack_reset)；地址含 CRLF 时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lsmtp_pack_rcpt(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    const char *rcpt = lpub_check_cstr(lua, 2);
    char *cmd = smtp_pack_rcpt(rcpt);
    return lpub_rtn_lud(lua, cmd, NULL != cmd ? strlen(cmd) : 0);
}
/// <summary>
/// 构造 SMTP DATA 命令（开始传输邮件内容）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针(释放同 pack_reset)</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lsmtp_pack_data(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    char *cmd = smtp_pack_data();
    return lpub_rtn_lud(lua, cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP QUIT 断连命令
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针(释放同 pack_reset)</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lsmtp_pack_quit(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    char *cmd = smtp_pack_quit();
    return lpub_rtn_lud(lua, cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP NOOP 心跳命令（保持连接）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针(释放同 pack_reset)</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lsmtp_pack_ping(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    char *cmd = smtp_pack_ping();
    return lpub_rtn_lud(lua, cmd, strlen(cmd));
}
LUAMOD_API int luaopen_smtp(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lsmtp_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "try_connect", _lsmtp_try_connect },
        { "check_code",_lsmtp_check_code },
        { "check_codes",_lsmtp_check_codes },
        { "check_ok", _lsmtp_check_ok },
        { "pack_reset", _lsmtp_pack_reset },
        { "pack_from", _lsmtp_pack_from },
        { "pack_rcpt", _lsmtp_pack_rcpt },
        { "pack_data", _lsmtp_pack_data },
        { "pack_quit", _lsmtp_pack_quit },
        { "pack_ping", _lsmtp_pack_ping },
        { "sock_id", _lsmtp_sock_id },
        { "__gc", _lsmtp_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_SMTP, reg_new, reg_func);
    return 1;
}
//srey.smtp.mail
/// <summary>
/// 创建邮件上下文，用于组装邮件内容
/// </summary>
/// <param>无</param>
/// <returns type="_smtp_mail_ctx">邮件对象</returns>
static int32_t _lsmtp_mail_new(lua_State *lua) {
    mail_ctx **ud = (mail_ctx **)lpub_push_ud(lua, NULL, MT_SMTP_MAIL);
    mail_ctx *mail;
    MALLOC(mail, sizeof(mail_ctx));
    mail_init(mail);
    *ud = mail;
    return 1;
}
/// <summary>
/// 释放邮件上下文内部资源（绑定为 __gc，由 Lua GC 自动调用）；重复调用安全
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lsmtp_mail_free(lua_State *lua) {
    mail_ctx **ud = lpub_check_udata(lua, 1, MT_SMTP_MAIL);
    if (NULL == *ud) {
        return 0;
    }
    mail_free(*ud);
    FREE(*ud);
    return 0;
}
/// <summary>
/// 设置组包时写不写 Reply-To 头(值是发件人地址)；只决定这一行头，挡不住收件人回复，见 C 层 mail_reply
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="reply" type="integer?">非 0 写 Reply-To 头；0 或 nil 不写回复头</param>
/// <returns>无</returns>
static int32_t _lsmtp_mail_reply(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    // 判 isnoneornil 而不是只判 LUA_TNIL:文档写的是"0 或 nil 不写回复头",而 mail:reply() 不带参数时
    // 类型是 LUA_TNONE,只判 TNIL 会落到 luaL_checkinteger 上报"number expected, got no value"
    int32_t reply = lua_isnoneornil(lua, 2) ? 0 : (0 != luaL_checkinteger(lua, 2));
    mail_reply(*ud, reply);
    return 0;
}
/// <summary>
/// 设置邮件主题
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="subject" type="string">邮件主题</param>
/// <returns>无</returns>
static int32_t _lsmtp_mail_subject(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *subject = lpub_check_cstr(lua, 2);
    mail_subject(*ud, subject);
    return 0;
}
/// <summary>
/// 设置邮件纯文本正文内容
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="msg" type="string">纯文本正文</param>
/// <returns>无</returns>
static int32_t _lsmtp_mail_msg(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *msg = lpub_check_cstr(lua, 2);
    mail_msg(*ud, msg);
    return 0;
}
/// <summary>
/// 设置邮件 HTML 正文内容
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="html" type="string">HTML 正文</param>
/// <returns>无</returns>
static int32_t _lsmtp_mail_html(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    size_t lens;
    const char *html = luaL_checklstring(lua, 2, &lens);
    mail_html(*ud, html, lens);
    return 0;
}
/// <summary>
/// 设置发件人姓名和邮箱地址
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="name" type="string">发件人显示名；最长 63 字节，超出按 UTF-8 字符边界截断</param>
/// <param name="email" type="string">发件人邮箱地址，最长 255 字节；超出则整个调用不生效(打 WARN)</param>
/// <returns>无</returns>
static int32_t _lsmtp_mail_from(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *name = lpub_check_cstr(lua, 2);
    const char *email = lpub_check_cstr(lua, 3);
    mail_from(*ud, name, email);
    return 0;
}
/// <summary>
/// 取发件人邮箱地址
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns type="string">发件人邮箱；没设过发件人时为空串。交出的是存量值——CRLF 已被剔除，
///   与调 from 时传进来的原串可能不同；超长地址根本不会被收下</returns>
static int32_t _lsmtp_mail_from_get(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    lua_pushstring(lua, (*ud)->from.addr);
    return 1;
}
/// <summary>
/// 添加收件人地址
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="email" type="string">收件人邮箱，最长 255 字节；超出不加入(打 WARN)</param>
/// <param name="type" type="integer">收件人类型，取值 [TO, BCC]（1/2/3），越界报错</param>
/// <returns>无</returns>
static int32_t _lsmtp_mail_addrs_add(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *email = lpub_check_cstr(lua, 2);
    mail_addr_type type = (mail_addr_type)lpub_check_range(lua, 3, TO, BCC, "mail address type out of range (TO/CC/BCC)");
    mail_addrs_add(*ud, email, type);
    return 0;
}
/// <summary>
/// 取全部收件人邮箱，含 TO / CC / BCC，按加入顺序
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns type="string[]">收件人邮箱数组；一个都没加时为空表。每个元素的口径同 from_get</returns>
static int32_t _lsmtp_mail_addrs_get(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    uint32_t n = maddr_arr_size(&(*ud)->addrs);
    lua_createtable(lua, (int32_t)n, 0);
    for (uint32_t i = 0; i < n; i++) {
        lua_pushstring(lua, maddr_arr_at(&(*ud)->addrs, (int32_t)i)->addr);
        lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
}
/// <summary>
/// 清空所有收件人列表
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lsmtp_mail_addrs_clear(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    mail_addrs_clear(*ud);
    return 0;
}
/// <summary>
/// 添加附件文件：当场读入，0 字节文件按空附件加入
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="file" type="string">附件文件路径</param>
/// <returns type="boolean">true 成功；不存在、不是常规文件或读失败返回 false（打 WARN，附件不加入）</returns>
static int32_t _lsmtp_mail_attach_add(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *file = lpub_check_cstr(lua, 2);
    return lpub_rtn_bool(lua, ERR_OK == mail_attach_add(*ud, file));
}
/// <summary>
/// 清空所有附件列表
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lsmtp_mail_attach_clear(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    mail_attach_clear(*ud);
    return 0;
}
/// <summary>
/// 清空邮件上下文：内容（主题 / 正文 / 发件人 / 收件人 / 附件）与 reply 标志一并还原到 mail_init 后的状态。
/// 注意 reply 会被复位为 1，同一对象复用时若不要 Reply-To 须在每次 clear 后重新调 reply(0)
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lsmtp_mail_clear(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    mail_clear(*ud);
    return 0;
}
/// <summary>
/// 将邮件上下文序列化为 MIME 格式内容字符串
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns type="lightuserdata?">MIME 字符串指针；生成 MIME boundary 取不到熵时返回 nil</returns>
/// <returns type="integer?">字符串长度；失败时两个返回值都是 nil，个数恒为 2</returns>
static int32_t _lsmtp_mail_pack(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    size_t lens = 0;
    char *content = mail_pack(*ud, &lens);
    return lpub_rtn_lud(lua, content, lens);// NULL 时压两个 nil
}
LUAMOD_API int luaopen_mail(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lsmtp_mail_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "reply", _lsmtp_mail_reply },
        { "subject",  _lsmtp_mail_subject },
        { "msg",  _lsmtp_mail_msg },
        { "html",  _lsmtp_mail_html },
        { "from",  _lsmtp_mail_from },
        { "from_get",  _lsmtp_mail_from_get },
        { "addrs_add",  _lsmtp_mail_addrs_add },
        { "addrs_get",  _lsmtp_mail_addrs_get },
        { "addrs_clear",  _lsmtp_mail_addrs_clear },
        { "attach_add",  _lsmtp_mail_attach_add },
        { "attach_clear",  _lsmtp_mail_attach_clear },
        { "clear",  _lsmtp_mail_clear },
        { "pack",  _lsmtp_mail_pack },
        { "__gc", _lsmtp_mail_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_SMTP_MAIL, reg_new, reg_func);
    return 1;
}
