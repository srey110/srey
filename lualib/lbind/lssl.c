#include "lbind/lpub.h"

#define CERT_FOLDER "keys" // SSL 证书文件所在子目录名
#define SECLEVEL_OUT_OF_RANGE "ssl security level out of range"
#define TLSVER_OUT_OF_RANGE "tls version out of range"

/// <summary>
/// 查询本次构建有没有把 SSL 编进来
/// </summary>
/// <returns type="boolean">编了 true；没编 false，此时 cert_register / p12_register 恒返 nil、
/// min_proto / host / core.send_ssl 恒返 false、seclevel / verify 是空操作</returns>
static int32_t _lssl_with_ssl(lua_State *lua) {
    return lpub_rtn_bool(lua, WITH_SSL);
}
#if WITH_SSL
// name 非空时把 cert 目录下的完整路径写进 out；为空则 out 保持调用方给的空串(表示不加载)。拼出来被截断返回 ERR_FAILED
static int32_t _cert_path(const char *name, char *out, size_t outlen) {
    if ('\0' == name[0]) {
        return ERR_OK;
    }
    int32_t n = SNPRINTF(out, outlen, "%s%s%s%s%s",
                         procpath(), PATH_SEPARATORSTR, CERT_FOLDER, PATH_SEPARATORSTR, name);
    if (!snprintf_ok(n, outlen)) {
        LOG_WARN("cert path too long: %s", name);
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 注册结果压栈:成功压 ssl 指针,ssl 没建出来或注册失败一律压 nil。
// 注册失败不必在这里释放 ssl —— evssl_register 每条失败路径都已 evssl_free
static int32_t _push_registered_ssl(lua_State *lua, const char *name, evssl_ctx *ssl) {
    if (NULL != ssl
        && ERR_OK == evssl_register(name, ssl)) {
        lua_pushlightuserdata(lua, ssl);
    } else {
        lua_pushnil(lua);
    }
    return 1;
}
#endif
/// <summary>
/// 加载 PEM/DER 格式的 CA、证书和私钥，按 name 注册 SSL 上下文。
/// 默认不验对端，安全级别用 OpenSSL 默认(1)：SHA1 这类弱摘要签的 CA 会被拒，要兼容就 seclevel(ctx, 0)。
/// 公网客户端须显式 verify 开对端校验、host 设主机名，再按需 seclevel / min_proto 加固(同 evssl_new)
/// </summary>
/// <param name="name" type="string">SSL 上下文注册名（字符串 key）</param>
/// <param name="ca" type="string">CA 文件名（相对 cert 目录）；空串表示不加载</param>
/// <param name="cert" type="string">证书文件名（相对 cert 目录）；空串表示不加载</param>
/// <param name="key" type="string">私钥文件名（相对 cert 目录）；空串表示不加载</param>
/// <param name="keytype" type="integer?">密钥格式，默认 SSL_FILETYPE_PEM</param>
/// <returns type="lightuserdata?">SSL 上下文指针；失败或未启用 SSL 时返回 nil</returns>
static int32_t _lssl_cert_register(lua_State *lua) {
#if WITH_SSL
    const char *name = lpub_check_cstr(lua, 1);
    const char *ca = lpub_check_cstr(lua, 2);
    const char *cert = lpub_check_cstr(lua, 3);
    const char *key = lpub_check_cstr(lua, 4);
    int32_t keytype = (int32_t)lpub_opt_range(lua, 5, SSL_FILETYPE_PEM, INT32_MIN, INT32_MAX, "keytype out of range");
    char capath[PATH_LENS] = { 0 };
    char certpath[PATH_LENS] = { 0 };
    char keypath[PATH_LENS] = { 0 };
    if (ERR_OK != _cert_path(ca, capath, sizeof(capath))
        || ERR_OK != _cert_path(cert, certpath, sizeof(certpath))
        || ERR_OK != _cert_path(key, keypath, sizeof(keypath))) {
        return lpub_rtn_nil(lua, 1);
    }
    return _push_registered_ssl(lua, name, evssl_new(capath, certpath, keypath, keytype));
#else
    return lpub_rtn_nil(lua, 1);
#endif
}
/// <summary>
/// 加载 PKCS12 格式证书文件，按 name 注册 SSL 上下文。默认校验与安全级别同 cert_register。
/// p12 里附带的 CA 只当自己要发出的证书链、不进信任库，
/// 用它开 SSL_VERIFY_PEER 验对端必然失败，要验对端用 cert_register 带 ca(同 evssl_p12_new)
/// </summary>
/// <param name="name" type="string">SSL 上下文注册名（字符串 key）</param>
/// <param name="p12" type="string">PKCS12 文件名（相对 cert 目录）</param>
/// <param name="pwd" type="string">PKCS12 文件密码</param>
/// <returns type="lightuserdata?">SSL 上下文指针；失败或未启用 SSL 时返回 nil</returns>
static int32_t _lssl_p12_register(lua_State *lua) {
#if WITH_SSL
    const char *name = lpub_check_cstr(lua, 1);
    const char *p12 = lpub_check_cstr(lua, 2);
    const char *pwd = lpub_check_cstr(lua, 3);
    char p12path[PATH_LENS] = { 0 };
    if (ERR_OK != _cert_path(p12, p12path, sizeof(p12path))) {
        return lpub_rtn_nil(lua, 1);
    }
    return _push_registered_ssl(lua, name, evssl_p12_new(p12path, pwd));
#else
    return lpub_rtn_nil(lua, 1);
#endif
}
/// <summary>
/// 按 name 查询已注册的 SSL 上下文
/// </summary>
/// <param name="name" type="string">SSL 上下文注册名</param>
/// <returns type="lightuserdata?">SSL 上下文指针；未找到（含名字内含 '\0'）或未启用 SSL 时返回 nil</returns>
static int32_t _lssl_query(lua_State *lua) {
#if WITH_SSL
    size_t lens;
    const char *name = luaL_checklstring(lua, 1, &lens);
    // 内含 '\0' 的名字注册不进来，按查不到算而不抛：srey.ssl_query 承诺查询不抛
    struct evssl_ctx *ssl = strlen(name) == lens ? evssl_query(name) : NULL;
    if (NULL != ssl) {
        lua_pushlightuserdata(lua, ssl);
    } else {
        lua_pushnil(lua);
    }
#else
    lua_pushnil(lua);
#endif
    return 1;
}
/// <summary>
/// 设置 SSL 安全级别
/// </summary>
/// <param name="evssl" type="lightuserdata">SSL 上下文指针</param>
/// <param name="level" type="integer">安全级别 0-5；越界报错</param>
/// <returns>无</returns>
static int32_t _lssl_seclevel(lua_State *lua) {
#if WITH_SSL
    LPUB_LUD_ARG(lua, struct evssl_ctx, 1, ssl);
    int32_t level = (int32_t)lpub_check_range(lua, 2, 0, 5, SECLEVEL_OUT_OF_RANGE);
    evssl_seclevel(ssl, level);
#else
    (void)lua;
#endif
    return 0;
}
/// <summary>
/// 设置最低 TLS 协议版本
/// </summary>
/// <param name="evssl" type="lightuserdata">SSL 上下文指针</param>
/// <param name="version" type="TLS_VERSION">
///        协议版本：
///            AUTO(0) 不设下限，用库支持的最低版本（OpenSSL 的默认状态，getter 也用 0 表示它）
///            TLS1_VERSION(0x0301)
///            TLS1_1_VERSION(0x0302)
///            TLS1_2_VERSION(0x0303)
///            TLS1_3_VERSION(0x0304)
///        表外的值报错
/// </param>
/// <returns type="boolean">设置成功 true；该版本被当前构建或安全级别排除时 false，此时最低版本
/// 保持原样。不判返回值就会以为下限抬上去了，实际仍在接受更低的版本。WITH_SSL 关闭时恒 false</returns>
static int32_t _lssl_min_proto(lua_State *lua) {
#if WITH_SSL
    LPUB_LUD_ARG(lua, struct evssl_ctx, 1, ssl);
    lua_Integer version = luaL_checkinteger(lua, 2);
    luaL_argcheck(lua, 0 == version
                  || (version >= TLS1_VERSION && version <= TLS1_3_VERSION),
                  2, TLSVER_OUT_OF_RANGE);
    return lpub_rtn_bool(lua, ERR_OK == evssl_min_proto(ssl, (int32_t)version));
#else
    return lpub_rtn_bool(lua, 0);
#endif
}
/// <summary>
/// 设置 SSL 上下文是否验证对端证书
/// </summary>
/// <param name="evssl" type="lightuserdata">SSL 上下文指针</param>
/// <param name="mod" type="integer">SSL_VERIFY_* 掩码：0 不验证 / SSL_VERIFY_PEER 验对端 / 叠加 SSL_VERIFY_FAIL_IF_NO_PEER_CERT 强制对端出证书(mTLS)</param>
/// <returns>无</returns>
static int32_t _lssl_verify(lua_State *lua) {
#if WITH_SSL
    LPUB_LUD_ARG(lua, struct evssl_ctx, 1, ssl);
    int32_t mod = lpub_check_i32(lua, 2, "verify mode out of range");
    evssl_verify(ssl, mod, NULL);
#else
    (void)lua;
#endif
    return 0;
}
/// <summary>
/// 设置客户端要连的主机名：握手时作 SNI 发出，开了 SSL_VERIFY_PEER 时还校验对端证书是签给它的，契约同 evssl_host。
/// 只开 verify 只验证书链，公网客户端还须设它。只给作客户端用的上下文设，须在上下文投入使用前设好
/// </summary>
/// <param name="evssl" type="lightuserdata">SSL 上下文指针</param>
/// <param name="host" type="string?">域名或 IP；nil 或空串清除</param>
/// <returns type="boolean">设置成功 true；超过 255 字节或 OpenSSL 不收这个名字(同 evssl_host)时 false，原设置不变。WITH_SSL 关闭时恒 false</returns>
static int32_t _lssl_host(lua_State *lua) {
#if WITH_SSL
    LPUB_LUD_ARG(lua, struct evssl_ctx, 1, ssl);
    const char *host = lua_isnoneornil(lua, 2) ? NULL : lpub_check_cstr(lua, 2);
    return lpub_rtn_bool(lua, ERR_OK == evssl_host(ssl, host));
#else
    return lpub_rtn_bool(lua, 0);
#endif
}
//srey.ssl
LUAMOD_API int luaopen_ssl(lua_State *lua) {
    luaL_Reg reg[] = {
        { "with_ssl", _lssl_with_ssl },
        { "cert_register", _lssl_cert_register },
        { "p12_register", _lssl_p12_register },
        { "query", _lssl_query },
        { "verify", _lssl_verify },
        { "host", _lssl_host },
        { "seclevel", _lssl_seclevel },
        { "min_proto", _lssl_min_proto },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
