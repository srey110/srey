local type = type
--任务名
TASK_NAME = {
	TAKS1 = 1,
	TAKS2 = 2,
	TAKS3 = 3
}
--ssl文件类型
SSL_FILETYPE = {
	PEM = 1,
	ASN1 = 2
}
--解包类型
UNPACK_TYPE = {
	NONE = 0,
	SIMPLE = 1,
}
--组包类型
PACK_TYPE = {
	NONE = 0,
	SIMPLE = 1,
}
function checkunptype(unptype)
	return nil == unptype and UNPACK_TYPE.NONE or unptype
end

ERR_OK = 0
INVALID_SOCK = -1

