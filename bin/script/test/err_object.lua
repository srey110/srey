-- 崩溃回归 helper：chunk 顶层抛一个非字符串错误对象。
-- lua_tostring 对它返回 NULL，直接喂给 LOG_ERROR 的 %s 是 UB。

error({ code = 42 })
