// 和网络相关的函数

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <stdint.h>    //uintptr_t
#include <stdarg.h>    //va_start....
#include <unistd.h>    //STDERR_FILENO等
#include <sys/time.h>  //gettimeofday
#include <time.h>      //localtime_r
#include <fcntl.h>     //open
#include <errno.h>     //errno

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"

// 构造函数
CSocket::CSocket()
{
    m_ListenPortCount = 1;      // 监听一个端口
    return;
}

// 析构函数
CSocket::~CSocket()
{
    // 释放必须的内存
    std::vector<lpngx_listening_t>::iterator pos;
    for(pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        delete (*pos); // 一定要把指针指向的内存进行释放，否者会造成内存泄漏
    }
    m_ListenSocketList.clear();
    return;
}

// 初始化函数【fork()子进程之前需要做的事】
bool CSocket::Initialize()
{
    bool reco = ngx_open_listening_sockets();
    return reco;
}

// 监听端口【支持多个端口】,这里遵从nginx官方的命名
// 在创建worker子进程之前就要执行这个函数
bool CSocket::ngx_open_listening_sockets()
{
    CConfig *p_config = CConfig::GetInstance();
    m_ListenPortCount = p_config->GetIntDefault("ListenPortCount", m_ListenPortCount);  // 取的要监听的端口数量

    int                 isock;              // socket
    struct sockaddr_in  serv_addr;          // 服务器的地址结构体
    int                 iport;              // 端口
    char                strinfo[100];       // 临时字符串


    // 初始化相关
    memset(&serv_addr, 0, sizeof(serv_addr));   // 初始化
    serv_addr.sin_family = AF_INET;             // 选择协议族为 ipv4
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 监听本地所有的IP地址，INADDR_ANY表示的是一个服务器上的所有网卡（服务器可能不止一个网卡）多个本地IP地址都能进行绑定端口号，进行侦听

    for(int i = 0; i < m_ListenPortCount; i++)  // 要监听这么多的端口
    {
        // 参数1：AF_INET: 使用IPV4协议，一般都这么写
        // 参数2：SOCK_STREAM   使用tcp，表示可靠连接【相对还有一个UDP套接字，表示不可靠连接】
        // 参数3：给0，固定用法
        isock = socket(AF_INET,SOCK_STREAM,0);  // 系统函数，成功返回非负描述符，出错返回-1
        if(isock == -1)
        {
            ngx_log_stderr(errno, "CSocket::Initialize()中socket()失败， i = %d.", i);
            // 其实这里直接退出，那如果已往有成功创建的socket呢？这样就会得不到释放，当然走到这里表示程序不正常，应该整体退出，也没有必要释放
            return false;
        }

        // setsockopt()：设置一些套接字参数选项
        // 参数2：是表示级别，和参数3配套使用，也就是说，参数3如果确定了，参数2就确定了
        // 参数3：允许重复用本地地址
        // 设置 SO_REUSEADDR。解决TIME_WAIT这个状态导致bind()失败的问题
        int reuseaddr = 1;  // 1:打开对应的设置项
        if(setsockopt(isock, SOL_SOCKET, SO_REUSEADDR, (const void *) &reuseaddr, sizeof(reuseaddr)) == -1)
        {
            ngx_log_stderr(errno, "CScoket::Initialize()中setsocopt(SO_REUSEADDR)失败，i = %d。", i);
            close(isock);   // 无需理会是否正常执行
            return false;
        }

        // 设置socket为非阻塞
        if(setnonblocking(isock) == false)
        {
            ngx_log_stderr(errno, "CSocekt::Initialize()中setnonblocking()失败,i=%d.",i);
            close(isock);
            return false;
        }

        // 设置本服务器要监听的地址和端口，这样客户端才能连接到该地址和端口并发送数据
        strinfo[0] = 0;
        sprintf(strinfo, "ListenPort%d", i);
        iport = p_config->GetIntDefault(strinfo, 10000);
        serv_addr.sin_port = htons((in_port_t)iport);   // in_port_t其实就是uint16_t

        // 绑定服务器地址结构体
        if(bind(isock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
        {
            ngx_log_stderr(errno, "CSocekt::Initialize()中bind()失败,i=%d.",i);
            close(isock);
            return false;
        }

        // 开始监听
        if(listen(isock, NGX_LISTEN_BACKLOG) == -1)
        {
            ngx_log_stderr(errno, "CSocekt::Initialize()中listen()失败,i=%d.",i);
            close(isock);
            return false;
        }

        // 放到列表里
        lpngx_listening_t p_listensocketitem = new ngx_listening_t;         // 注意不要写错，前面类型是指针类型，后面的类型是一个结构体
        memset(p_listensocketitem, 0, sizeof(ngx_listening_t));             // 这里后面用的是 ngx_listening_t 而不是lpngx_listening_t
        p_listensocketitem->port = iport;                                   // 记录所监听的端口号
        p_listensocketitem->fd = isock;                                     // 保存套接字句柄
        ngx_log_error_core(NGX_LOG_INFO, 0, "监听%d端口成功！", iport);      // 打印日志
        m_ListenSocketList.push_back(p_listensocketitem);                   // 加入到队列中
    }
    return true;

}

// 设置socket连接为非阻塞模式【这种函数写法很固定】。非阻塞：【不断调用，不断调用这种，在拷贝数据的时候是阻塞的】
bool CSocket::setnonblocking(int sockfd)
{
    int nb = 1;     // 0:清除   1：设置
    if (ioctl(sockfd, FIONBIO, &nb) == -1)
    {
        return false;
    }
    return true;

    // 如下也是一种写法，跟上面这种写法其实是一样的，只是上面这个写法更简单
    // fcntl:file control 【文件控制】相关函数，执行各种描述符控制操作
    // 参数1：所要设置的秒数符，这里是套接字【也是描述符的一种】
    // int opts = fcntl(sockfd, F_GETFL);  // 用F_GETL先获取描述符的一些标志信息
    // if(opts < 0)
    // {
    //     ngx_log_stderr(errno,"CSocekt::setnonblocking()中fcntl(F_GETFL)失败.");
    //     return false;
    // }
    // opts |= O_NONBLOCK; // 把非阻塞标记加到原来的标记上，标记这是一个非租塞套接字，【如何关闭非阻塞呢？opts &= ~O_NONBLOCK;然后再F_SETFL 一下即可】
    // if(fcntl(sockfd, F_SETFL, opts) < 0)
    // {
    //     ngx_log_stderr(errno,"CSocekt::setnonblocking()中fcntl(F_SETFL)失败.");
    //     return false;
    // }
    // return true;
    
}

// 关闭socket，什么时候用，这里暂时不确定，先把这个函数预备在这里
void CSocket::ngx_close_listening_sockts()
{
    for(int i = 0; i < m_ListenPortCount; i++)  // 要关闭这些个监听端口
    {
        close(m_ListenSocketList[i]->fd);
        ngx_log_error_core(NGX_LOG_INFO,0,"关闭监听端口%d!",m_ListenSocketList[i]->port); //显示一些信息到日志中
    }
    return;
}