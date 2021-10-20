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

// #include <sys/socket.h>
#include <sys/ioctl.h>  // ioctl
#include <arpa/inet.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"

// 构造函数
CSocket::CSocket()
{
    // 配置相关
    m_worker_connections = 1;       // epoll连接的最大项数
    m_ListenPortCount = 1;          // 监听一个端口

    // epoll相关
    m_epollhandle = -1;             // epoll返回的句柄
    m_pconnections = NULL;          // 连接池【连接数组】先置空
    m_pfree_connections = NULL;     // 连接池中的空闲连接链

    // m_pread_events = NULL;          // 读事件数组给空
    // m_pwrite_events = NULL;         // 写事件数组给空
    return;
}

// 析构函数
CSocket::~CSocket()
{
    // 释放必须的内存
    // 1）监听端口的释放---------------------------------------
    std::vector<lpngx_listening_t>::iterator pos;
    for(pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        delete (*pos); // 一定要把指针指向的内存进行释放，否者会造成内存泄漏
    }
    m_ListenSocketList.clear();

    // 2）连接池相关的内容释放-------------------------
    // if(m_pwrite_events != NULL) // 释放写事件数组
    // {
    //     delete []m_pwrite_events;
    // }

    // if(m_pread_events != NULL)  // 释放读事件数组
    // {
    //     delete []m_pread_events;
    // }

    if(m_pconnections != NULL)  // 释放连接池
    {
        delete []m_pconnections;
    }

    return;
}

// 初始化函数【fork()子进程之前需要做的事】
// 成功返回true,失败返回false
bool CSocket::Initialize()
{
    ReadConf();     // 读配置项
    bool reco = ngx_open_listening_sockets();   // 打开监听窗口
    return reco;
}

// 专门用于读各种配置项
void CSocket::ReadConf()
{
    CConfig *p_config = CConfig::GetInstance();
    m_worker_connections = p_config->GetIntDefault("worker_connections", m_worker_connections); // epoll连接的最大项数
    m_ListenPortCount    = p_config->GetIntDefault("ListenPortCount", m_ListenPortCount);       // 取得所要监听的端口数量
    return;
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

    // 需要用到的一些配置信息
    CConfig *p_config = CConfig::GetInstance();

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
    if(m_ListenSocketList.size() <= 0)
        return false;
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
        //ngx_log_stderr(0,"端口是%d,socketid是%d.",m_ListenSocketList[i]->port,m_ListenSocketList[i]->fd);
        close(m_ListenSocketList[i]->fd);
        ngx_log_error_core(NGX_LOG_INFO,0,"关闭监听端口%d!",m_ListenSocketList[i]->port); //显示一些信息到日志中
    }
    return;
}

// *-*-*---*-*-*************************************************--------------------------------------
// 1)epoll功能初始化，子进程中进行，本函数被ngx_worker_process_init()调用
int CSocket::ngx_epoll_init()
{
    // 1）很多内核版本不处理epoll——create参数，知道该参数 > 0 即可
    // 创建一个epoll对象，创建了一个红黑树，还创建了一个双向链表
    m_epollhandle = epoll_create(m_worker_connections);     // 直接以epoll连接的最大项数为参数，肯定满足大于0
    if(m_epollhandle == -1)
    {
        ngx_log_stderr(errno, "CSocket::ngx_epoll_init()中epoll_create()失败。");
        exit(2);    // 这个是致命问题。直接退出系统。资源交由系统进行释放
    }

    // 2）创建连接池【数组】、创建出来，这个东西用于后续处理所有客户端的连接
    m_connection_n = m_worker_connections;      // 记录当前连接池中的连接总数
    // 连接池【数组，每个元素时一个对象】
    m_pconnections = new ngx_connection_t[m_connection_n];      // new是不可能失败，这里不用判断，如果失败，直接报异常会更好
    // m_pread_events = new ngx_event_t[m_connection_n];
    // m_pwrite_events = new mgx_event_t[m_connection_n];
    // for(int i = 0; i < m_connection_n; i++)
    // {
    //     m_pconnections[i].instance = 1; // 失效标志位设置为1,
    // }

    int i = m_connection_n;             // 连接池中的连接数
    lpngx_connection_t next = NULL;
    lpngx_connection_t c = m_pconnections;  // 连接池数组的首地址

    do
    {
        i--;                    // 注意i是数字末尾，从最后遍历，i递减至数组的首个元素

        // 从尾部往头部走----------------------
        c[i].data = next;       // 设置连接对象的next指针，注意第一次循环时next = NULL
        c[i].fd = -1;           // 初始化连接，无socket和该连接池中的连接【对象】绑定
        c[i].instance = 1;      // 失效标志位设置为1,【失效】
        c[i].iCurrsequence = 0; // 当前序号统一从 0 开始

        // -----------------------------------

        next = &c[i];           // next指针向前移

    } while (i);    // 循环直到 i 为 0 .即循环到数组首地址

    m_pfree_connections = next;         // 设置空闲连接链表头指针，因为现在next指向c[0]，注意现在整个链表都是空的
    m_free_connection_n = m_connection_n;   // 空闲连接链表的长度，因为现在整个链表都是空的，所以这两个参数相等

    // 3）遍历所有监听socket【监听端口】，我们为每个监听socket增加一个 连接池 中的连接。说白了，就是让一个socket和一个内存绑定，以方便记录该socket相关的数据，状态等
    std::vector<lpngx_listening_t>::iterator pos;
    for(pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        c = ngx_get_connection((*pos)->fd);     // 从连接池中获取一个空闲连接对象
        if(c == NULL)
        {
            // 这是致命问题，刚开始怎么可能连接池就为空呢？
            ngx_log_stderr(errno,"CSocekt::ngx_epoll_init()中ngx_get_connection()失败.");
            exit(2); // 致命问题，直接退，交给系统处理释放
        }
        c->listening = (*pos);      // 连接对象 和 监听对象关联，方便通过连接对象找到监听对象
        (*pos)->connection = c;     // 监听对象 和 连接对象关联，方便通过监听对象找到连接对象

        // rev->accept = 1;         // 监听端口必须设置accept标志为1

        // 对于监听端口的读事件设置处理方法，因为监听端口是用来等待对象连接的发送三次握手的，所以监听端口关心的就是【读事件】
        c->rhandler = &CSocket::ngx_event_accept;

        // 往监听socket上增加监听事件，从而开始让监听端口履行其职责【如果不加这行，虽然端口能连接上，但是不会触发ngx_epoll_process_events()里面的epoll_wait()往下走
        // ngx_epoll_add_event参数
        // (*pos)->fd,              socket句柄
        // 1, 0,                    读， 写 【只关心读事件，所以参数2：readevent = 1，而参数3：writeevent = 0;】
        // 0,                       其他事件补充标记
        // EPOLL_CTL_ADD,           事件类型【增加，还有其他事件 MOV(修改),DEL(删除)】
        // c                        连接池中的连接
        if(ngx_epoll_add_event((*pos)->fd, 1, 0, 0, EPOLL_CTL_ADD, c) == -1)
        {
            exit(2); //有问题，直接退出，日志 已经写过了
        }
    }
    
    return 1;

}

// 2)监听端口开始工作，必须为其增加读事件，因为监听端口只关心读事件
// void CSocket::ngx_epoll_listenportstart()
// {
//     std::vector<lpngx_listening_t>::iterator pos;
//     for(pos = m_ListenSocketList.begin(); pos != m_ListenSocket.end(); ++pos)
//     {
//         ngx_epoll_add_event((*pos)->fd, 1,0);       // 只关心读事件
//     }
//     return;
// }


// epoll增加事件，可能被ngx_epoll_init()等函数调用
// fd:句柄，一个socket
// readevent：表示是否是一个读事件，0是，1不是
// writeevent：表示是否是一个写事件，0是，1不是
// otherflag：其他需要额外补充的标记
// eventtype：事件类型，一帮用的就是系统的枚举值， 增加，删除，修改等
// c：对应的连接池中的连接的指针
// 返回值：成功返回1，失败返回-1
int CSocket::ngx_epoll_add_event(int fd, int readevent, int writeevent, uint32_t otherflag, uint32_t eventtyple, lpngx_connection_t c)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    if(readevent == 1)
    {
        // 读事件，这里发现官方nginx没有使用EPOLLERR，所以这里我也不使用
        ev.events = EPOLLIN|EPOLLRDHUP; // EPOLLIN读事件，也就是read ready【客户端三次握手连接进来，也属于一种可读事件】 EPOLLRDHUP 客户端关闭连接，断联
                                        // 这里似乎不用加EPOLLERR,只用EPOLLRDHUP即可，EPOLLERR/EPOLLRDHUP实际上是通过触发读写事件进程读写操作 recv write来检测连接异常

        // ev.events |= (ev.events | EPOLLET); // 只支持费阻塞socket的高速模式【ET：边缘触发】，就拿accept来说，如果加这个EPOLLET，则客户端连入时，epoll_wait()只会返回一次该事件
                                            // 如果用的是EPOLLLT【LT：水平触发】，则客户端连入时，epoll_wait()就会被触发多次，一直到用accept()来处理

        //https://blog.csdn.net/q576709166/article/details/8649911
        // 关于EPOLLERR的一些说法
        // 1）对端正常关闭（程序里close(),shell下kill或者ctrl+c），触发EPOLLIN和EPOLLRDHUP，但是不触发EPOLLERR 和 EPOLLHUP
        // 2）EPOLLRDHUP    这个好像系统检测不到，可以使用EPOLLIN，read返回0，删除掉事件，关闭close(fd)；如果有EPOLLRDHUP，检测他就可以知道是对方关闭，否者就用上面的方法
        // 3）client端close()连接，server会报某个sockfd可读，即epollin来临，然后recv一下，如果返回0在调用epoll_stl中的EPOLL_CTL_DEL,同时关闭close(sockfd);
        // 有些系统会收到一个EPOLLRDHUP，当然检测这个是最好不过，只可惜是有些系统。所以使用上面的方法最保险，如果能加上对EPOLLRDHUP的处理那就是万能的了
        // 4）EPOLLERR  只有采取动作时，才能知道是否对方异常，即如果对方突然断掉，那是不可能有此事件发生的。只有自己采取动作（当然自己此时也不知道）read,write时，出EPOLLERR错，说明对方已经异常断开
        // 5）EPOLLERR  是服务器这边出错（自己出错能检测到）
        // 6）给已经关闭的socket写时，会发生EPOLLERR，也就是说，只有在采取行动（比如：读一个已经关闭的socket，或者写一个已经关闭的socket）的时候，才知道对方是否已经关闭了
        // 这个时候，如果对方异常关闭了，则会出现EPOLLERR，出现Error把对方DEL掉，close就可以

    }
    else
    {
        // 其他事件类型
    }

    if(otherflag != 0)
    {
        ev.events |= otherflag;
    }

    // 以下代码出自官方nginx，因为指针的最后一位【二进制位】肯定不是1，所以，和c->instance做 | 运算；到时候通过一些编码，即可以取得C的真实地址，又可以把此时此刻的c->instance值取到
    // 比如c是个地址，可能的值是 0x00af0578, 对应的二进制是‭101011110000010101111000，而 | 1 后，是0x00af0579
    ev.data.ptr = (void *)((uintptr_t)c | c->instance);     // 把对象弄进去，后续来事件时，用epoll_wait()后，这个对象就能被取出来
                                                            // 但同时把一个标志位【不是0就是1】弄进去

    if(epoll_ctl(m_epollhandle, eventtype,fd,&ev) == -1)
    {
        ngx_log_stderr(errno, "CSocekt::ngx_epoll_add_event()中epoll_ctl(%d,%d,%d,%u,%u)失败.",fd,readevent,writeevent,otherflag,eventtype);
        // exit(2); // 致命问题，直接退出，资源交由系统释放，后来发现不能直接退
        return -1;
    }

    return 1;

}


// 开始获取发生的事件消息
// 参数 unsigned int timer - epoll_wait() 阻塞的时长，单位是 毫秒
// 返回值，1：正常返回，0：有问题返回，一帮不管是正常还是问题返回，都应该保持进程继续运行
// 本函数被ngx_process_events_and_timers()调用，而ngx_process_events_and_timers()是在子进程的死循环中被反复调用
int CSocket::ngx_epoll_process_events(int timer)
{
    // 等待事件，事件会返回到m_events里，最多返回NGX_MAX_EVENTS个事件【因为我们这里只提供了这些内存】
    // 阻塞timer这么长的时间，除非：1）：阻塞时间到达，2）阻塞期间收到了事件会立即返回，3）调用的时候有事件也会立即返回，4）如果来个信号，比如使用kill -1 pid测试
    // 如果timer为-1则一直阻塞，如果timer为0则立即返回，即便没有任何事件
    // 返回值：  有错误发生返回-1，错误在errno中，比如：你发送了一个信号过来，就返回-1，错误信息（4：Interrupted system call)
    //          如果你等待的是一段时间，并且超时了，则返回0
    //          如果返回 > 0 则表示成功捕获到这么多个事件【返回值里】
    int events = epoll_wait(m_epollhandle,m_events,NGX_MAX_EVENTS,timer);

    if(events == -1)
    {
        // 有错误发生，发送某个信号给本进程，就可以导致这个条件成立，而且错误码为 4
        // #define EINTR 4, EINTR错误的产生：当阻塞于某个慢系统调用的一个进程捕获某个信号且响应的信号处理函数返回时，该系统调用可能会返回一个EINTR错误
        // 例如：在socket服务器端，设置了信号捕获机制，有子进程，在父进程阻塞于慢系统调用时，由于父进程捕获到一个有效的信号，内核会致使accept返回一个EINTR错误（被中断的系统调用）
        if(errno == EINTR)
        {
            // 信号所导致，直接返回，一般认为这不是什么毛病，但是还是打印日志记录一下，因为一般也不会认为给worker进程发送消息
            ngx_log_error_core(NGX_LOG_INFO,errno,"CSocekt::ngx_epoll_process_events()中epoll_wait()失败!");
            return 1;   // 正常返回
        }
        else
        {
            // 这里被认为应该是有问题的，记录日志
            ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocekt::ngx_epoll_process_events()中epoll_wait()失败!");
            return 0;   // 非正常返回
        }
    }

    if(events == 0)     // 超时，但是事件没来
    {
        if(timer != -1)
        {
            // 要求epoll_wait阻塞一定的时间而不是一直阻塞，这属于是阻塞时间到了，正常返回
            return 1;
        }
        // 无限等待【所以不存在超时】，但是却没有任何返回事件，这应该不正常有问题
        ngx_log_error_core(NGX_LOG_ALERT,0,"CSocekt::ngx_epoll_process_events()中epoll_wait()没超时却没返回任何事件!"); 
        return 0; //非正常返回 
    }

    // 会惊群，一个telnet上来，4个worker进程都会被惊动，都执行下面这个
    // ngx_log_stderr(errno,"惊群测试1:%d",events); 

    // 走到这里，就属于有事件收到了
    lpngx_connection_t      c;
    uintptr_t               instance;
    uint32_t                revents;
    for(int i = 0; i < events; ++i)     // 遍历本次epoll_wait返回的所有事件，注意，events才是返回的实际事件数量
    {
        c = (lpngx_connection_t)(m_events[i].data.ptr);         // ngx_epoll_add_event()给进去的，这里就能取出来
        instance = (uintptr_t)c & 1;                            // 将地址的最后一位取出来，用instance变量进行标识，见：ngx_epoll_add_event函数。该值当时是随着连接池中的连接一起给传进来的
        c = (lpngx_connection_t)((uintptr_t) c & (uintptr_t) ~1);   // 最后一位去掉，得到真正的c地址

        // 仔细分析一下官方nginx这个判断
        // 一个套接字，当关联一个连接池中的连接【对象】时，这个套接字值是要给到c->fd的
        // 那么什么时候这个c->fd会变成-1呢？关闭连接时，这个fd会被设置为-1,具体哪行代码设置的-1后续再看，但是这里应该不是ngx_free_connection()函数设置的-1
        if(c->fd == -1)
        {
            // 比如我们使用epoll_wait取得三个事件，处理第一个事件时，因为业务需要，我们把这个链接关闭，那么我们应该会把c->fd设置为-1；
            // 第二个事件照常处理
            // 第三个事件：假如这个第三个事件也跟第一个事件对应的是同一个连接，那么这个条件就会成立，那么这种事件就属于过期事件，不应该处理

            // 这里可以增加个日志，
            ngx_log_error_core(NGX_LOG_DEBUG,0,"CSocekt::ngx_epoll_process_events()中遇到了fd=-1的过期事件:%p.",c); 
            continue; //这种事件就不处理即可
        }

        if(c->instance != instance)
        {
            // 什么时候这个条件成立呢？【换种问法：instance标志为什么可以判断事件是否过期？】
            // 比如我们使用epoll_wait取的第单个事件，处理第一个事件时，因为业务需要，我们把这个链接关闭【麻烦就麻烦在这个连接被服务器关闭上了】，但是恰好第三个事件也和这个连接有关；
            // 因为第一个事件就把socket连接关闭了，显然第三个事件我们是我们不应该处理的【因为这是一个过期事件】，若处理，肯定会导致错误。
            
        }
    }


}


