#ifndef MYSQL_MACRO_H_
#define MYSQL_MACRO_H_

#define INT3_MAX                  0xFFFFFF

#define MYSQL_OK                  0x00
#define MYSQL_EOF                 0xfe
#define MYSQL_ERR                 0xff
#define MYSQL_QUIT                0x01
#define MYSQL_INIT_DB             0x02
#define MYSQL_QUERY               0x03
#define MYSQL_PING                0x0e
#define MYSQL_STMT_PREPARE        0x16
#define MYSQL_STMT_EXECUTE        0x17
#define MYSQL_STMT_SEND_LONG_DATA 0x18
#define MYSQL_STMT_CLOSE          0x19
#define MYSQL_STMT_RESET          0x1a

#endif//MYSQL_MACRO_H_
