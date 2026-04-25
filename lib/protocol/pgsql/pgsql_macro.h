#ifndef PGSQL_MACRO_H_
#define PGSQL_MACRO_H_

// 数据包类型
typedef enum pgpack_type {
    PGPACK_OK = 0x00,           // 命令执行成功
    PGPACK_ERR,                 // ErrorResponse：服务端返回错误
    PGPACK_NOTIFICATION         // NotificationResponse：异步通知
}pgpack_type;

// 数据格式
typedef enum pgpack_format {
    FORMAT_TEXT = 0,            // 文本格式
    FORMAT_BINARY               // 二进制格式
}pgpack_format;

// PostgreSQL 内置类型 OID 常量
#define BOOLOID 16              // bool
#define BYTEAOID 17             // bytea
#define CHAROID 18              // char
#define NAMEOID 19              // name
#define INT8OID 20              // int8
#define INT2OID 21              // int2
#define INT2VECTOROID 22        // int2vector
#define INT4OID 23              // int4
#define REGPROCOID 24           // regproc
#define TEXTOID 25              // text
#define OIDOID 26               // oid
#define TIDOID 27               // tid
#define XIDOID 28               // xid
#define CIDOID 29               // cid
#define OIDVECTOROID 30         // oidvector
#define JSONOID 114             // json
#define XMLOID 142              // xml
#define PG_NODE_TREEOID 194     // pg_node_tree
#define PG_NDISTINCTOID 3361    // pg_ndistinct
#define PG_DEPENDENCIESOID 3402 // pg_dependencies
#define PG_MCV_LISTOID 5017     // pg_mcv_list
#define PG_DDL_COMMANDOID 32    // pg_ddl_command
#define XID8OID 5069            // xid8
#define POINTOID 600            // point
#define LSEGOID 601             // lseg
#define PATHOID 602             // path
#define BOXOID 603              // box
#define POLYGONOID 604          // polygon
#define LINEOID 628             // line
#define FLOAT4OID 700           // float4
#define FLOAT8OID 701           // float8
#define UNKNOWNOID 705          // unknown
#define CIRCLEOID 718           // circle
#define MONEYOID 790            // money
#define MACADDROID 829          // macaddr
#define INETOID 869             // inet
#define CIDROID 650             // cidr
#define MACADDR8OID 774         // macaddr8
#define ACLITEMOID 1033         // aclitem
#define BPCHAROID 1042          // bpchar（定长字符串）
#define VARCHAROID 1043         // varchar（变长字符串）
#define DATEOID 1082            // date
#define TIMEOID 1083            // time
#define TIMESTAMPOID 1114       // timestamp
#define TIMESTAMPTZOID 1184     // timestamptz（带时区时间戳）
#define INTERVALOID 1186        // interval
#define TIMETZOID 1266          // timetz（带时区时间）
#define BITOID 1560             // bit
#define VARBITOID 1562          // varbit
#define NUMERICOID 1700         // numeric
#define REFCURSOROID 1790       // refcursor
#define REGPROCEDUREOID 2202    // regprocedure
#define REGOPEROID 2203         // regoper
#define REGOPERATOROID 2204     // regoperator
#define REGCLASSOID 2205        // regclass
#define REGCOLLATIONOID 4191    // regcollation
#define REGTYPEOID 2206         // regtype
#define REGROLEOID 4096         // regrole
#define REGNAMESPACEOID 4089    // regnamespace
#define UUIDOID 2950            // uuid
#define PG_LSNOID 3220          // pg_lsn（日志序列号）
#define TSVECTOROID 3614        // tsvector（全文搜索向量）
#define GTSVECTOROID 3642       // gtsvector
#define TSQUERYOID 3615         // tsquery（全文搜索查询）
#define REGCONFIGOID 3734       // regconfig
#define REGDICTIONARYOID 3769   // regdictionary
#define JSONBOID 3802           // jsonb
#define JSONPATHOID 4072        // jsonpath
#define TXID_SNAPSHOTOID 2970   // txid_snapshot
#define PG_SNAPSHOTOID 5038     // pg_snapshot
#define INT4RANGEOID 3904       // int4range
#define NUMRANGEOID 3906        // numrange
#define TSRANGEOID 3908         // tsrange
#define TSTZRANGEOID 3910       // tstzrange
#define DATERANGEOID 3912       // daterange
#define INT8RANGEOID 3926       // int8range
#define INT4MULTIRANGEOID 4451  // int4multirange
#define NUMMULTIRANGEOID 4532   // nummultirange
#define TSMULTIRANGEOID 4533    // tsmultirange
#define TSTZMULTIRANGEOID 4534  // tstzmultirange
#define DATEMULTIRANGEOID 4535  // datemultirange
#define INT8MULTIRANGEOID 4536  // int8multirange
#define RECORDOID 2249          // record
#define RECORDARRAYOID 2287     // record 数组
#define CSTRINGOID 2275         // cstring
#define ANYOID 2276             // any
#define ANYARRAYOID 2277        // anyarray
#define VOIDOID 2278            // void
#define TRIGGEROID 2279         // trigger
#define EVENT_TRIGGEROID 3838   // event_trigger
#define LANGUAGE_HANDLEROID 2280    // language_handler
#define INTERNALOID 2281        // internal
#define ANYELEMENTOID 2283      // anyelement
#define ANYNONARRAYOID 2776     // anynonarray
#define ANYENUMOID 3500         // anyenum
#define FDW_HANDLEROID 3115     // fdw_handler
#define INDEX_AM_HANDLEROID 325 // index_am_handler
#define TSM_HANDLEROID 3310     // tsm_handler
#define TABLE_AM_HANDLEROID 269 // table_am_handler
#define ANYRANGEOID 3831        // anyrange
#define ANYCOMPATIBLEOID 5077   // anycompatible
#define ANYCOMPATIBLEARRAYOID 5078      // anycompatiblearray
#define ANYCOMPATIBLENONARRAYOID 5079   // anycompatiblenonarray
#define ANYCOMPATIBLERANGEOID 5080      // anycompatiblerange
#define ANYMULTIRANGEOID 4537           // anymultirange
#define ANYCOMPATIBLEMULTIRANGEOID 4538 // anycompatiblemultirange
#define PG_BRIN_BLOOM_SUMMARYOID 4600           // pg_brin_bloom_summary
#define PG_BRIN_MINMAX_MULTI_SUMMARYOID 4601    // pg_brin_minmax_multi_summary

// 以下为数组类型 OID
#define BOOLARRAYOID 1000
#define BYTEAARRAYOID 1001
#define CHARARRAYOID 1002
#define NAMEARRAYOID 1003
#define INT8ARRAYOID 1016
#define INT2ARRAYOID 1005
#define INT2VECTORARRAYOID 1006
#define INT4ARRAYOID 1007
#define REGPROCARRAYOID 1008
#define TEXTARRAYOID 1009
#define OIDARRAYOID 1028
#define TIDARRAYOID 1010
#define XIDARRAYOID 1011
#define CIDARRAYOID 1012
#define OIDVECTORARRAYOID 1013
#define PG_TYPEARRAYOID 210
#define PG_ATTRIBUTEARRAYOID 270
#define PG_PROCARRAYOID 272
#define PG_CLASSARRAYOID 273
#define JSONARRAYOID 199
#define XMLARRAYOID 143
#define XID8ARRAYOID 271
#define POINTARRAYOID 1017
#define LSEGARRAYOID 1018
#define PATHARRAYOID 1019
#define BOXARRAYOID 1020
#define POLYGONARRAYOID 1027
#define LINEARRAYOID 629
#define FLOAT4ARRAYOID 1021
#define FLOAT8ARRAYOID 1022
#define CIRCLEARRAYOID 719
#define MONEYARRAYOID 791
#define MACADDRARRAYOID 1040
#define INETARRAYOID 1041
#define CIDRARRAYOID 651
#define MACADDR8ARRAYOID 775
#define ACLITEMARRAYOID 1034
#define BPCHARARRAYOID 1014
#define VARCHARARRAYOID 1015
#define DATEARRAYOID 1182
#define TIMEARRAYOID 1183
#define TIMESTAMPARRAYOID 1115
#define TIMESTAMPTZARRAYOID 1185
#define INTERVALARRAYOID 1187
#define TIMETZARRAYOID 1270
#define BITARRAYOID 1561
#define VARBITARRAYOID 1563
#define NUMERICARRAYOID 1231
#define REFCURSORARRAYOID 2201
#define REGPROCEDUREARRAYOID 2207
#define REGOPERARRAYOID 2208
#define REGOPERATORARRAYOID 2209
#define REGCLASSARRAYOID 2210
#define REGCOLLATIONARRAYOID 4192
#define REGTYPEARRAYOID 2211
#define REGROLEARRAYOID 4097
#define REGNAMESPACEARRAYOID 4090
#define UUIDARRAYOID 2951
#define PG_LSNARRAYOID 3221
#define TSVECTORARRAYOID 3643
#define GTSVECTORARRAYOID 3644
#define TSQUERYARRAYOID 3645
#define REGCONFIGARRAYOID 3735
#define REGDICTIONARYARRAYOID 3770
#define JSONBARRAYOID 3807
#define JSONPATHARRAYOID 4073
#define TXID_SNAPSHOTARRAYOID 2949
#define PG_SNAPSHOTARRAYOID 5039
#define INT4RANGEARRAYOID 3905
#define NUMRANGEARRAYOID 3907
#define TSRANGEARRAYOID 3909
#define TSTZRANGEARRAYOID 3911
#define DATERANGEARRAYOID 3913
#define INT8RANGEARRAYOID 3927
#define INT4MULTIRANGEARRAYOID 6150
#define NUMMULTIRANGEARRAYOID 6151
#define TSMULTIRANGEARRAYOID 6152
#define TSTZMULTIRANGEARRAYOID 6153
#define DATEMULTIRANGEARRAYOID 6155
#define INT8MULTIRANGEARRAYOID 6157
#define CSTRINGARRAYOID 1263

#endif//PGSQL_MACRO_H_
