# Srey
c lua(opt)lightweight server framework.   
support IOCP、EPOLL、KQUEUE、EVPORT、POLLSET、DEVPOLL、
SSL、Http、Websocket、MQTT、DNS、MySql、PostgreSQL、Redis...

## Compile     
* windows use vs2015;   
* linux、unix run mk.sh.

## Configuration  
* config.json Configure service startup parameters. 

## Use(eg httpd)
* LUA   
  create http_sv.lua：
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
  then register the service in startup.lua(task.register("test.http_sv", TASK_NAME.httpsv))   
* C    
  create http_sv.h http_sv.c：   
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
  then register the service in startup.h   
