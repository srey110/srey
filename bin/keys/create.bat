@echo off
rem 证书落在本脚本所在目录(bin/keys)，从哪里运行都一样
cd /d "%~dp0"
rem 任一步 openssl 失败即停，不拿残缺文件往下签 (同 create.sh)
set encry=sha256
set p12psw=srey
set list=server client

rem 生成根证书私钥
openssl genrsa -out ca.key 2048 || (pause & exit /b 1)
rem 生成根证书签发申请文件
openssl req -new -key ca.key -out ca.csr -subj "/C=CN/ST=SC/L=CD/O=Organization/OU=srey/CN=srey@gmail.com" || (pause & exit /b 1)
rem 自签发根证书(带 CA:TRUE 扩展,否则 OpenSSL 不认作合法 CA,mTLS 验对端时报 invalid CA)
echo basicConstraints=critical,CA:TRUE>ca_ext.cnf
echo keyUsage=critical,keyCertSign,cRLSign>>ca_ext.cnf
openssl x509 -req -days 3650 -%encry% -signkey ca.key -in ca.csr -out ca.crt -extfile ca_ext.cnf || (pause & exit /b 1)
del ca_ext.cnf
echo "-----------------ca finished--------------------"
rem pkcs12 显式用 AES：OpenSSL 1.1 默认是 RC2-40，OpenSSL 3 不加载 legacy 读不了
rem CN 用合法主机名：用例拿它测按 CN 比对主机名，OpenSSL 4 设主机名时拒收 @ 和 _
for %%a in (%list%) do (
	openssl genrsa -out %%a.key 2048 || (pause & exit /b 1)
	openssl req -new -key %%a.key -out %%a.csr -subj "/C=CN/ST=SC/L=CD/O=Organization%%a/OU=srey%%a/CN=srey-%%a" || (pause & exit /b 1)
	openssl x509 -req -days 3650 -%encry% -CA ca.crt -CAkey ca.key -CAserial serial.srl -CAcreateserial -in %%a.csr -out %%a.crt || (pause & exit /b 1)
	openssl verify -CAfile ca.crt %%a.crt || (pause & exit /b 1)
	openssl pkcs12 -export -inkey %%a.key -in %%a.crt -certfile ca.crt -password pass:%p12psw% -certpbe AES-256-CBC -keypbe AES-256-CBC -macalg sha256 -out %%a.p12 || (pause & exit /b 1)
	openssl pkcs12 -in %%a.p12 -password pass:%p12psw% -noout -info || (pause & exit /b 1)
	echo "-----------------%%a finished--------------------"
)
pause
