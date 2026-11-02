#!/bin/sh
# 证书落在本脚本所在目录(bin/keys)，从哪里运行都一样；任一步失败即停，不拿残缺文件往下签
cd "$(dirname "$0")" || exit 1
set -e
encry=sha256
p12psw=srey
list="server client"

# 生成根证书私钥
openssl genrsa -out ca.key 2048
# 生成根证书签发申请文件
openssl req -new -key ca.key -out ca.csr -subj "/C=CN/ST=SC/L=CD/O=Organization/OU=srey/CN=srey@gmail.com"
# 自签发根证书(带 CA:TRUE 扩展,否则 OpenSSL 不认作合法 CA,mTLS 验对端时报 invalid CA)
cat > ca_ext.cnf <<EOF
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign
EOF
openssl x509 -req -days 3650 -"$encry" -signkey ca.key -in ca.csr -out ca.crt -extfile ca_ext.cnf
rm -f ca_ext.cnf
echo "-----------------ca finished--------------------"

for a in $list; do
    openssl genrsa -out "$a.key" 2048
    # CN 用合法主机名：用例拿它测按 CN 比对主机名，OpenSSL 4 设主机名时拒收 @ 和 _
    openssl req -new -key "$a.key" -out "$a.csr" -subj "/C=CN/ST=SC/L=CD/O=Organization$a/OU=srey$a/CN=srey-$a"
    openssl x509 -req -days 3650 -"$encry" -CA ca.crt -CAkey ca.key -CAserial serial.srl -CAcreateserial -in "$a.csr" -out "$a.crt"
    openssl verify -CAfile ca.crt "$a.crt"
    # 显式用 AES：LibreSSL 与 OpenSSL 1.1 默认是 RC2-40，OpenSSL 3 不加载 legacy 读不了
    openssl pkcs12 -export -inkey "$a.key" -in "$a.crt" -certfile ca.crt -password pass:"$p12psw" -certpbe AES-256-CBC -keypbe AES-256-CBC -macalg sha256 -out "$a.p12"
    openssl pkcs12 -in "$a.p12" -password pass:"$p12psw" -noout -info
    echo "-----------------$a finished--------------------"
done
