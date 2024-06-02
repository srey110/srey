#ifndef MYSQL_MACRO_H_
#define MYSQL_MACRO_H_

#define MYSQL_HEAD_LENS   4
#define MYSQL_PACK_OK     0x00
#define MYSQL_PACK_EOF    0xfe
#define MYSQL_PACK_ERR    0xff
#define MYSQL_CLIENT_CAPS 227271

#define COM_QUIT 0x01
#define COM_INIT_DB 0x02
#define COM_QUERY 0x03
#define COM_PING 0x0e
#define COM_STMT_PREPARE 0x16
#define COM_STMT_EXECUTE 0x17
#define COM_STMT_SEND_LONG_DATA 0x18
#define COM_STMT_CLOSE 0x19
#define COM_STMT_RESET 0x1a

#define MYSQL_CACHING_SHA2     "caching_sha2_password"
#define MYSQL_NATIVE_PASSWORLD "mysql_native_password"

//Capabilities Flags
#define CLIENT_CONNECT_WITH_DB  8 //Database (schema) name can be specified on connect in Handshake Response Packet.
#define CLIENT_PROTOCOL_41      512 //New 4.1 protocol. 
#define CLIENT_SSL              2048 //Use SSL encryption for the session.
#define CLIENT_RESERVED2        32768 //DEPRECATED: Old flag for 4.1 authentication \ CLIENT_SECURE_CONNECTION. 
#define CLIENT_PLUGIN_AUTH      (1UL << 19) //Client supports plugin authentication. 

#endif//MYSQL_MACRO_H_
