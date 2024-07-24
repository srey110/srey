# srey
c lua (可选)跨平台服务器框架.   
支持IOCP、EPOLL、KQUEUE、EVPORT、POLLSET、DEVPOLL网络模型;   
支持SSL、Http、Websocket、DNS、MySql、Redis等

## 编译     
* windows使用vs2015;   
* linux、unix执行mk.sh编译.

## 配置文件  
* config.json 文件配置服务启动参数. 

## 使用(以http时间显示为例)
* lua 实现   
  创建http_sv.lua 并写入如下代码：
  ```lua
  local srey = require("lib.srey")
  local http = require("lib.http")
  
  srey.startup(
    function ()
        srey.on_recved(
            function (pktype, fd, skid, client, slice, data, size)
                http.response(fd, skid, 200, nil, os.date("%Y-%m-%d %H:%M:%S", os.time()))
            end
        )
        srey.listen(PACK_TYPE.HTTP, 0, "0.0.0.0", 80)
    end
  )
  ```
  然后在startup.lua注册该服务(task.register("test.http_sv", TASK_NAME.httpsv))即可   
* c 实现   
  创建http_sv.h http_sv.c写入如下代码：   
  ```c
  static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, 
      size_t size) {
      binary_ctx bwriter;
      binary_init(&bwriter, NULL, 0, 0);
      http_pack_resp(&bwriter, 200);
      char time[TIME_LENS];
      sectostr(nowsec(), "%Y-%m-%d %H:%M:%S", time);
      http_pack_content(&bwriter, time, strlen(time));
      ev_send(&task->loader->netev, fd, skid, bwriter.data, bwriter.offset, 0);
  }
  static void _startup(task_ctx *task) {
      on_recved(task, _net_recv);
      uint64_t id;
      task_listen(task, PACK_HTTP, NULL, "0.0.0.0", 80, &id, 0);
  }
  void http_sv(loader_ctx *loader, name_t name) {
      task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
      task_register(task, _startup, NULL);
  }
  ```
  然后在startup.h注册该服务(http_sv(...))即可
  更多功能请参考代码... 
