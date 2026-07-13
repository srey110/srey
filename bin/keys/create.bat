@echo off
set encry=sha256
set p12psw=srey
set list=server client

rem 生成根证书私钥
openssl genrsa -out ca.key 2048
rem 生成根证书签发申请文件
openssl req -new -key ca.key -out ca.csr -subj "/C=CN/ST=SC/L=CD/O=Organization/OU=srey/CN=srey@gmail.com"
rem 自签发根证书(带 CA:TRUE 扩展,否则 OpenSSL 不认作合法 CA,mTLS 验对端时报 invalid CA)
echo basicConstraints=critical,CA:TRUE>ca_ext.cnf
echo keyUsage=critical,keyCertSign,cRLSign>>ca_ext.cnf
openssl x509 -req -days 3650 -%encry% -signkey ca.key -in ca.csr -out ca.crt -extfile ca_ext.cnf
del ca_ext.cnf
echo "-----------------ca finished--------------------"
for %%a in (%list%) do (
	openssl genrsa -out %%a.key 2048
	openssl req -new -key %%a.key -out %%a.csr -subj "/C=CN/ST=SC/L=CD/O=Organization%%a/OU=srey%%a/CN=srey_%%a@gmail.com"
	openssl x509 -req -days 3650 -%encry% -CA ca.crt -CAkey ca.key -CAserial serial.srl -CAcreateserial -in %%a.csr -out %%a.crt
	openssl verify -CAfile ca.crt %%a.crt
	openssl pkcs12 -export -inkey %%a.key -in %%a.crt -certfile ca.crt -password pass:%p12psw% -out %%a.p12
	openssl pkcs12 -in %%a.p12 -password pass:%p12psw% -noout -info
	echo "-----------------%%a finished--------------------"
)
pause
