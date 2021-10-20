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
            
            
        }
        

    } while (1);
    
}