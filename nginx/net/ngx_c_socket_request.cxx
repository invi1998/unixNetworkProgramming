
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
#include "ngx_c_memory.h"

//来数据时候的处理，当连接上有数据来的时候，本函数会被ngx_epoll_process_events()所调用  ,官方的类似函数为ngx_http_wait_request_handler();
void CSocket::ngx_wait_request_handler(lpngx_connection_t c)
{
    // 收包，注意这里使用的第二个和第三个参数，我这里用的始终都是这俩个参数，所以必须要保证 c->precvbuf 指向正确的收包位置，保证c->irecvlen指向正确的收包宽度
    ssize_t reco = recvproc(c, c->precvbuf, c->irecvlen);
    if (reco <= 0)
    {
        return;     // 该处理的上面这个recvproc()函数里处理过了，这里<=直接退出
    }

    // 走到这里，说明成功收到了一些字节（>0 ）,就要开始判断收到多少数据了
    if (c->curStat == _PKG_HD_INIT)         // 连接建立起来肯定是这个状态，因为在ngx_get_connection()中已经把curStat成员赋值为_PKG_HD_INIT了
    {
        if (reco == m_iLenPkgHeader)        // 正好接收到完整包头，这里拆解包头
        {
            ngx_wait_request_handler_proc_p1(c);    // 调用专门针对包头处理完整的函数进行处理
        }
        else
        {
            // 收到的包头不完整--我们不可能预料每个包的长度，也不能预料各种拆包/粘包的情况，所以收不到完整的包头【也算是缺包】是有可能的
            c->curStat = _PKG_HD_RECVING;           // 接收包头中，包头不完整，继续接收包头
            c->precvbuf = c->precvbuf + reco;       // 注意接收后续包的内存往后走
            c->irecvlen = c->irecvlen - reco;       // 要接收的内容当然也要减少，以确保只收到完整的包头先

        }
        
    }
    else if (c->curStat == _PKG_HD_RECVING)         // 接收包头中，包头不完整，继续接收中，这个条件才会成立
    {
        if (c->irecvlen == reco) // 要求收到的宽度和我们实际收到的宽度相等
        {
            // 包头收完整了
            ngx_wait_request_handler_proc_p1(c);    // 调用专门针对包头处理的函数
        }
        else
        {
            // 包头还没有收完整，继续收包头
            // c->curStat = _PKG_HD_RECVING;        // 这里没有必要
            c->precvbuf = c->precvbuf + reco;       // 注意收后续包的内存往后走
            c->irecvlen = c->irecvlen - reco;       // 要收的内容自然要减少，以确保只收到完整的包头先

        }
        
    }
    else if (c->curStat == _PKG_BD_INIT)
    {
        // 包头刚好收完，准备收包体
        if (reco == c->irecvlen)
        {
            // 收到的宽度等于要接收的宽度，包体也收完整了
            ngx_wait_request_handler_proc_plast(c);
        }
        else
        {
            // 收到的宽度小于要收的宽度
            c->curStat = _PKG_BD_RECVING;
            c->precvbuf = c->precvbuf + reco;
            c->irecvlen = c->irecvlen - reco;
        }
        
    }
    else if (c->curStat == _PKG_BD_RECVING)
    {
        // 接收包体中，包体不完整，继续接收中
        if (c->irecvlen == reco)
        {
            // 包体接收完整了
            ngx_wait_request_handler_proc_plast(c);
        }
        else
        {
            // 包体没接收完整，继续接收
            c->precvbuf = c->precvbuf + reco;
			c->irecvlen = c->irecvlen - reco;
        }
        
    }

    return;
}

// 接收数据专用函数，引入这个函数是为了方便，如果断线，错误子类的，这里直接 释放连接池中的连接，然后直接关闭socket，以免在其他函数中还要重复干这些是
// 参数c：连接池中的相关连接
// 参数buff：接收数据的缓冲区
// 参数buflen：要接收的数据大小
// 返回值：返回-1   则是有问题并且已经在这里把问题处理完毕了，本函数的调用者一般是可以直接return不做处理
        // 返回>0   则是表示实际收到的字节数
ssize_t CSocket::recvproc(lpngx_connection_t c, char *buff, ssize_t buflen)     // ssize_t是有符号整形，在32位机器上等同于int，在64位机器上等同于long int,  size_t就是无符号型的ssize_t
{
    ssize_t n;

    n= recv(c->fd, buff, buflen, 0);    // recv()系统函数。最后一个参数flag，一般为0
    if (n == 0)
    {
        // 客户端关闭【应该是正常完成了4次挥手】，这里就直接回收连接，关闭socket
        // ngx_log_stderr(0,"连接被客户端正常关闭[4路挥手关闭]！");
        ngx_close_connection(c);
        return -1;
    }

    // 客户端没断，走到这里
    if (n < 0)  // 这里被认为有错误发生
    {
        // EAGAIN和EWOULDBLOCK【这个应该常用在hp上】应该是一样的值，表示没有收到数据。一般来讲，在ET模式下会出现这个错误，因为ET模式下是不停的recv，肯定有一个时刻会收到这个errno，但是LT模式下一般是来事件才会收，所以不会出现这个返回值
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // 这里认为LT模式不该出现这个errno，而且其实这个也不是一个错误，所以不当错误进行处理
            ngx_log_stderr(errno,"CSocket::recvproc()中errno == EAGAIN || errno == EWOULDBLOCK成立，出乎我意料！");//epoll为LT模式不应该出现这个返回值，所以直接打印出来瞧瞧
            return -1; //不当做错误处理，只是简单返回
        }

        // EINTR错误的产生：当阻塞于某个慢系统调用的一个进程捕获某个信号并且相应的信号处理函数返回时，该系统调用可能返回一个EINTR错误
        // 例如：在socket服务器端，设置了信号捕获机制，有子进程，当在父进程阻塞于慢系统调用时，由父进程吧捕获到了一个有效信号时，内核会致使accept返回一个EINTR错误（被中断的系统调用）
        if (errno == EINTR)     // 这个不算错误，参考官方nginx
        {
            //我认为LT模式不该出现这个errno，而且这个其实也不是错误，所以不当做错误处理
            ngx_log_stderr(errno,"CSocekt::recvproc()中errno == EINTR成立，出乎我意料！");//epoll为LT模式不应该出现这个返回值，所以直接打印出来瞧瞧
            return -1; //不当做错误处理，只是简单返回
        }

        // 所有从这里走下来的错误，都认为是异常：意味着我们要关闭客户端套接字，要回收连接池中的连接
        
        
    }
    
    

}