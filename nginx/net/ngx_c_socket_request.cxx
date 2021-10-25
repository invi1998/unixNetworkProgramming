
//和网络  中 客户端请求数据有关的代码

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>    //uintptr_t
#include <stdarg.h>    //va_start....
#include <unistd.h>    //STDERR_FILENO等
#include <sys/time.h>  //gettimeofday
#include <time.h>      //localtime_r
#include <fcntl.h>     //open
#include <errno.h>     //errno
//#include <sys/socket.h>
#include <sys/ioctl.h> //ioctl
#include <arpa/inet.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"

//来数据时候的处理，当连接上有数据来的时候，本函数会被ngx_epoll_process_events()所调用  ,官方的类似函数为ngx_http_wait_request_handler();
void CSocekt::ngx_wait_request_handler(lpngx_connection_t c)
{
    // 三次握手连接进来，当客户端发送数据进来的时候，就会触发该函数
    //ngx_log_stderr(errno,"22222222222222222222222.");
    // ET模式测试代码
    // unsigned char buf[10] = {0};
    // memset(buf, 0, sizeof(buf));
    // do
    // {
    //     int n = recv(c->fd, 2, 0);      // 每次只接收两个字节
    //     if (n == -1 && errno == EAGAIN)
    //     {
    //         break;
    //     }
    //     else if (n == 0)
    //     {
    //         break;
    //     }
    //     ngx_log_stderr(0, "成功， 收到的字节数为%d, 内容为%s", n, buf);
    // } while (1);
    
    // LT模式测试代码
    // 水平触发如果没有正确处理客户端关闭情况，那么水平触发就会在客户端关闭的时候不断的触发
    // 因为客户端关闭也是可读
    unsigned char buf[10] = {0};
    memset(buf, 0, sizeof(buf));
    int n = recv(c->fd, 2, 0);      // 每次只接收两个字节
    if (n == 0)
    {
        // 连接关闭
        ngx_free_connection(c);
        close(c->fd);
        c->fd = -1;
    }
    ngx_log_stderr(0, "成功， 收到的字节数为%d, 内容为%s", n, buf);

    return;
}