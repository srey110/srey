-- 初始化 MySQL：补建 test1 库；建立 bin/script/test/db_mysql.lua 用到的 test_bind 表。
-- test 库与 admin 用户已由 docker-compose 的 MYSQL_DATABASE / MYSQL_USER 自动创建。
-- 用户使用默认的 caching_sha2_password 插件。srey 明文连接不做 RSA 公钥交换，服务端缓存冷时(如容器重启后)
-- 完整认证须走 TLS；C / Lua 测试都先经 TLS 登录一次，之后的明文连接走缓存的快速认证。

-- 安装 query_attributes 组件，提供 mysql_query_attribute_string() 等函数，
-- 测试代码（task_mysql.c / bin/script/test/db_mysql.lua）的 INSERT 使用它读取绑定参数。
INSTALL COMPONENT 'file://component_query_attributes';

CREATE DATABASE IF NOT EXISTS test1
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
GRANT ALL PRIVILEGES ON test1.* TO 'admin'@'%';
GRANT ALL PRIVILEGES ON test.*  TO 'admin'@'%';
FLUSH PRIVILEGES;

USE test;
CREATE TABLE IF NOT EXISTS test_bind (
    id          BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    t_int8      TINYINT      NULL,
    t_int16     SMALLINT     NULL,
    t_int32     INT          NULL,
    t_int64     BIGINT       NULL,
    t_float     FLOAT        NULL,
    t_double    DOUBLE       NULL,
    t_string    VARCHAR(255) NULL,
    t_datetime  DATETIME     NULL,
    t_time      TIME         NULL,
    t_nil       VARCHAR(64)  NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
