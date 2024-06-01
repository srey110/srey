#ifndef MYSQL_MACRO_H_
#define MYSQL_MACRO_H_

#define MYSQL_HEAD_LENS   4
#define MYSQL_PACK_OK     0x00
#define MYSQL_PACK_EOF    0xfe
#define MYSQL_PACK_ERR    0xff
#define MYSQL_CLIENT_CAPS 227271

#define MYSQL_CACHING_SHA2     "caching_sha2_password"
#define MYSQL_NATIVE_PASSWORLD "mysql_native_password"

//Capabilities Flags
#define CLIENT_CONNECT_WITH_DB  8 //Database (schema) name can be specified on connect in Handshake Response Packet.
#define CLIENT_PROTOCOL_41      512 //New 4.1 protocol. 
#define CLIENT_SSL              2048 //Use SSL encryption for the session.
#define CLIENT_RESERVED2        32768 //DEPRECATED: Old flag for 4.1 authentication \ CLIENT_SECURE_CONNECTION. 
#define CLIENT_PLUGIN_AUTH      (1UL << 19) //Client supports plugin authentication. 

#endif//MYSQL_MACRO_H_
