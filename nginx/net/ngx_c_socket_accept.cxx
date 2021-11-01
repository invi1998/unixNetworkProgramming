// 和网络连接中 接受连接【accept】有关的函数放在这里

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

// 建立新连接专用函数，当新连接进入时，本函数会被ngx_epoll_process_events()调用
void CSocket::ngx_event_accept(lpngx_connection_t oldc)
{
    // 因为listen套接字上用的不是ET【边缘触发】，而是LT【水平触发】,意味着客户端连入如果我不要处理，那这个函数会被多次调用，所以这里可以不必多次accept()，可以只执行一次accept()
    // 这也可以避免本函数被卡很久，注意：本函数应该尽快返回，以免阻塞程序运行
    struct sockaddr         mysockaddr;         // 远端服务器的socket地址
    socklen_t               socklen;
    int                     err;
    int                     level;
    int                     s;
    static  int             use_accept4 = 1;    // 我们先认为他能够使用accept4()函数
    lpngx_connection_t      newc;               // 代表连接池中一个连接【注意这里是指针】

    // ngx_log_stderr(0, "这是几个\n"); 这里会惊群，也就是说，epoll技术本身会有惊群的问题

    socklen = sizeof(mysockaddr);
    do      // 用do，跳到while后边去会更方便
    {
        if(use_accept4)
        {
            s = accept4(oldc->fd, &mysockaddr, &socklen,SOCK_NONBLOCK);     // 从内核中获取一个用户端连接，最后一个参数 SOCK_NONBLOCK表示返回一个非阻塞的socket，节省一次ioctl【设置非阻塞】调用
        }
        else
        {
            s = accept(old->fd,&mysockaddr, &socklen,SOCK_NONBLOCK);
        }

        //惊群，有时候不一定完全惊动所有4个worker进程，可能只惊动其中2个等等，其中一个成功其余的accept4()都会返回-1；错误 (11: Resource temporarily unavailable【资源暂时不可用】) 
        //所以参考资料：https://blog.csdn.net/russell_tao/article/details/7204260
        //其实，在linux2.6内核上，accept系统调用已经不存在惊群了（至少我在2.6.18内核版本上已经不存在）。大家可以写个简单的程序试下，在父进程中bind,listen，然后fork出子进程，
               //所有的子进程都accept这个监听句柄。这样，当新连接过来时，大家会发现，仅有一个子进程返回新建的连接，其他子进程继续休眠在accept调用上，没有被唤醒。
        //ngx_log_stderr(0,"测试惊群问题，看惊动几个worker进程%d\n",s); 【我的结论是：accept4可以认为基本解决惊群问题，但似乎并没有完全解决，有时候还会惊动其他的worker进程】

        if (s == 1)
        {
            err = errno;

            // 对于accept,send和recv而言，事件未发生时，errno通常会被设置为EAGAIN（意为“再来一次）或者EWOULDBLOCK（意为“期待阻塞”）
            if(err == EAGAIN)   // accept()没准备好，这个EAGAIN错误EWOULDBLOCK是一样的
            {
                // 除非你用一个循环不断的accept()取走所有的连接，不然一般不会有这个错误【我们在这里只取一个连接】
                return;
            }
            level = NGX_LOG_ALERT;
            if (err == ECONNABORTED)    // ECONNABORTED错误则发生在对方意外关闭套接字后【您的主机中的软件放弃了一个已经建立的连接--由于超时或者其他失败而终止连接（用户插拔网线就可能导致这个错误出现】
            {
                // 该错误被描述为“software caused connection abort” ， 即：软件引起的连接中断；原因在于当前服务和客户进程在完成用于TCP连接的“三次握手”后
                // 客户端 TCP 却发送了一个 RST （复位）分节，在服务器进程看来，就在该连接已经由TCP 排队，等待服务进程调用 accept 的时候 RST 却到达了。
                // POSIX 规定此时的 errno 值必须为 ECONNABORTED。源自 Berkeley 的实现完全在内核中处理终止的连接，服务器进程将永远不知道该终止的发生
                // 服务器进程一般可以忽略该错误，直接再次调用accept

                level = NGX_LOG_ERR;
            }

            // EMFILE: 进程的fd已经用尽【已达到系统所允许单一进程所能打开的文件/套接字总数】。
            // 使用命令 ulimit -n  ： 查看文件描述符显示，如果是1024的话，需要改大：打开的文件句柄数过多，把系统的fd软限制和硬限制都抬高
            // ENFILE：这个errno的存在，表明一定存在system-wide的resource limits，而不仅仅有process-specific的resouce limits。按照常识，
            // process-soecific的resource limits一定受限于system-wide的resource limits
            else if (err == EMFILE || err == ENFILE)
            {
                level = NGX_LOG_CRIT;
            }

            ngx_log_error_core(level, errno, "CSocket::ngx_event_accept()中accept4()失败!");

            if (use_accept4 && err == ENOSYS)   // accept4()函数没实现
            {
                use_accept4 = 0;                // 标记不使用accept4()函数，改用accept()函数
                continue;
            }

            if (err == ECONNABORTED)            // 对方关闭套接字
            {
                // 这个错误因为可以忽略，所以不用干啥

            }

            if (err == EMFILE || err == ENFILE)
            {
                // do nothing 官方nginx的做法是，先把读事件从listen socket中移除，然后再弄一个定时器，定时器到了则继续执行该函数，但是定时器到了有个标记，会把读事件增加到listen socket上去
                // 这里先暂时不处理，因为上面即写这个日志了
            }
            return;
            
        }

        // 走到这里，表示accept4()成功了
        // ngx_log_stderr(errno, "accept4成功s=%d", s);    // s这里就是一个句柄了
        newc = ngx_get_connection(s);
        if (newc == NULL)
        {
            // 连接池中的连接不够用，那么就得把这个socket直接关闭并返回了，因为ngx_get_connection()中已经写日志了，所以这里不再需要写日志了
            if (close(s) == -1)
            {
                ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocket::ngx_event_accept()中close(%d)失败！", s);
            }
            return;
            
        }

        // ---------------将来这里会判断是否连接超过最大允许连接数，现在先暂时不处理

        // 成功拿到连接池中一个连接
        memset(&newc->s_sockaddr, &mysockaddr, socklen);    // 拷贝客户端地址到连接对象【要转成字符串IP地址，参考函数ngx_sock_ntop()】

        // {
        //     // 测试将收到的地址弄成字符串，格式形如“192.168.1.126"或者""192.168.1.126:40904"
        //     u_char ipaddr[100];
        //     memset(ipaddr,0, sizeof(ipaddr));
        //     ngx_sock_ntop(&newc->s_sockaddr, 1, ipaddr, sizeof(ipaddr)-10); // 宽度给小点
        //     ngx_log_stderr(0, "ip信息为%s\n", ipaddr);
        // }
        
        if(!use_accept4)
        {
            // 如果不是用accept4()取得的socket，那么就要设置为非阻塞【因为使用accept4()的已经被accept4()设置为非阻塞了】
            if (setnonblocking(s) == false)
            {
                // 设置非阻塞失败
                ngx_close_connection(newc);
                return;     // 直接返回
            }
            
        }

        newc->listening = oldc->listening;                      // 连接对象，和监听对象关联，方便通过连接对象找监听对象【关联到监听端口】
        // newc->w_ready = 1;                                      // 标记可以写，新连接写事件肯定是ready的，【从连接池拿出一个连接时这个连接的所有成员都是0】
        newc->rhandler = &CSocket::ngx_wait_request_handler;    // 设置数据来的时候的读处理函数。其实官方nginx中是ngx_http_wait_request_handler()

        // 客户端应该主动发送第一次的数据，这里读事件加入epoll监控
        // s,                   socket句柄
        // 1, 0,                读， 写
        // EPOLLET,             其他补充标记【EPOLLET（高速模式，边缘触发ET）】
        // EPOLL_CTL_ADD,       事件类型【增加，还有删除/修改】
        // newc                 连接池中的连接
        // if (ngx_epoll_add_event(s, 1, 0, EPOLLET, EPOLL_CTL_ADD, newc) == -1)    // ET
        // if (ngx_epoll_add_event(s, 1, 0, 0, EPOLL_CTL_ADD, newc) == -1) // LT 本项目使用LT模式
        // s,                   socket句柄
        // EPOLL_CTL_ADD,       事件类型，这里是增加
        // EPOLLIN|EPOLLRDHUP,  标志，这里代表要增加的标志，EPOLLIN:可读， EPOLLRDHUP: tcp连接的远端关闭或者半关闭
        // 0,                   对于事件类型为增加的，不需要这个参数
        // newc                 连接池中的连接
        if (ngx_epoll_oper_event(s, EPOLL_CTL_ADD, EPOLLIN|EPOLLRDHUP,0,newc) == -1)
        {
            // 增加事件失败。失败日志在ngx_epoll_add_event中写过了
            ngx_close_connection(newc);
            return; // 直接返回
        }

        break;      // 一般就是循环一次就跳出去
    
    } while (1);
    
    return;

}
