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
#include <sys/ioctl.h>  // ioctl
#include <arpa/inet.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"


// 构造函数
CSocket::CSocket()
{
    // 配置相关
    m_worker_connections = 1;       // epoll连接的最大项数
    m_ListenPortCount = 1;          // 监听一个端口
    m_RecyConnectionWaitTime = 60;  // 等待这么些秒后才回收连接

    // epoll相关
    m_epollhandle = -1;             // epoll返回的句柄
    // m_pconnections = NULL;          // 连接池【连接数组】先置空
    // m_pfree_connections = NULL;     // 连接池中的空闲连接链
    // m_pread_events = NULL;          // 读事件数组给空
    // m_pwrite_events = NULL;         // 写事件数组给空

    //一些和网络通讯有关的常用变量值，供后续频繁使用时提高效率
    m_iLenPkgHeader = sizeof(COMM_PKG_HEADER);    //包头的sizeof值【占用的字节数】
    m_iLenMsgHeader =  sizeof(STRUC_MSG_HEADER);  //消息头的sizeof值【占用的字节数】

    // 多线程相关
    //pthread_mutex_init(&m_recvMessageQueueMutex, NULL); //互斥量初始化 

    // 各种队列相关
    m_iSendMsgQueueCount            = 0;        // 发消息队列大小
    m_total_recyconnection_n        = 0;        // 待释放连接队列大小
    m_cur_size_                     = 0;        // 当前计时队列尺寸
    m_timer_value_                  = 0;        // 当前计时队列头部的时间值

    // 在线用户相关
    m_onlineUserCount               = 0;        // 在线用户数量统计，先给0

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

    return;
}

// 初始化函数【fork()子进程之前需要做的事】
// 成功返回true,失败返回false
// todo 之前是在master进程进行套接字监听，现在计划将其迁移到worker子进程中
bool CSocket::Initialize()
{
    ReadConf();     // 读配置项
    if (ngx_open_listening_sockets() == false)  // 打开监听端口
    {
        return false;
    }
    return true;

}

// 子进程中才需要执行的初始化函数
bool CSocket::Initialize_subproc()
{
    // 发消息互斥量初始化
    if (pthread_mutex_init(&m_sendMessageQueueMutex, NULL) != 0)
    {
        ngx_log_stderr(0, "CSocket::Initialize_subproc()中pthread_mutex_init(&m_sendMessageQueueMutex)失败.");
        return false;
    }

    // 连接相关互斥量初始化
    if (pthread_mutex_init(&m_connctionMutex, NULL) != 0)
    {
        ngx_log_stderr(0,"CSocket::Initialize_subproc()中pthread_mutex_init(&m_connectionMutex)失败.");
        return false; 
    }

    // 连接回收队列相关互斥量初始化
    if (pthread_mutex_init(&m_recyconnqueueMutex, NULL) != 0)
    {
        ngx_log_stderr(0,"CSocket::Initialize_subproc()中pthread_mutex_init(&m_recyconnqueueMutex)失败.");
        return false; 
    }

    // 和事件处理队列有关的互斥量初始化
    if (pthread_mutex_init(&m_timequeueMutex, NULL) != 0)
    {
        ngx_log_stderr(0,"CSocket::Initialize_subproc()中pthread_mutex_init(&m_timequeueMutex)失败.");
        return false;
    }
    

    // 初始化发消息相关信号量，信号量用于 进程 /  线程 之间的同步，虽然 互斥量【pthread_mutex_lock】和条件变量【pthread_cond_wait】都是线程之间的同步手段
    // 但是这里用信号量实现，则 跟容易理解，跟容易简化问题，使用信号量书写的代码短小且清晰
    // 第二个参数 = 0；表示信号量在线程之间共享，确实如此，如果非 0 ，表示在进程之间共享
    // 第三个参数 = 0；表示信号量的初始值，为 0 时，调用sem_wait()就会卡在那里等待
    if (sem_init(&m_semEventSendQueue, 0, 0) == -1)
    {
        ngx_log_stderr(0,"CSocket::Initialize_subproc()中sem_init(&m_semEventSendQueue,0,0)失败.");
        return false;
    }
    
    // 创建线程
    int err;
    ThreadItem *pSendQueue;                                                                     // 专门用来发送数据的线程
    m_threadVector.push_back(pSendQueue = new ThreadItem(this));                                // 创建一个新的线程对象并放到容器中
    err = pthread_create(&pSendQueue->_Handle, NULL, ServerSendQueueThread, pSendQueue);        // 创建线程， 错误不返回到errno，一般返回错误码
    if (err!=0)
    {
        return false;
    }

    ThreadItem *pRecyconn;                                                                          // 专门用来回收连接的线程
    m_threadVector.push_back(pRecyconn = new ThreadItem(this));
    err = pthread_create(&pRecyconn->_Handle, NULL, ServerRecyConnectionThread, pRecyconn);         // 创建线程，错误错误不返回errno，一般返回错误码
    if (err != 0)
    {
        return false;
    }

    if (m_ifkickTimeCount == 1) // 是否开启踢人时钟， 1：开启， 0：不开启
    {
        ThreadItem *pTimemonitor;       // 专门用来处理到期不发心跳包的用户踢出的线程
        m_threadVector.push_back(pTimemonitor = new ThreadItem(this));
        err = pthread_create(&pTimemonitor->_Handle, NULL, ServerTimerQueueMonitorThread, pTimemonitor);
        if(err != 0)
        {
            ngx_log_stderr(0,"CSocket::Initialize_subproc()中pthread_create(ServerTimerQueueMonitorThread)失败.");
            return false;
        }
    }

    return true;
    
}

// 关闭退出函数【子进程中执行】
void CSocket::Shutdown_subproc()
{
    // （1）把干活的线程停止掉，注意 系统应该尝试通过设置 g_stopEvent = 1 来让整个项目停止
    // 用到信号量的，可能还需要调用一下sem_post
    if (sem_post(&m_semEventSendQueue) == -1)   // 让ServerSendQueueThread()流程走下来干活
    {
        ngx_log_stderr(0,"CSocket::Shutdown_subproc()中sem_post(&m_semEventSendQueue)失败.");
    }
    
    std::vector<ThreadItem*>::iterator iter;
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); ++iter)
    {
        pthread_join((*iter)->_Handle, NULL);   // 等待一个线程终止
    }
    
    // （2）释放一下new出来的ThreadItem【线程池中的线程】
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); ++iter)
    {
        if (*iter)
        {
            delete *iter;
        }
        
    }

    m_threadVector.clear();
    
    // （3）队列相关
    clearMsgSendQueue();
    clearconnection();
    clearAllFromTimerQueue();

    // (4)多线程相关
    pthread_mutex_destroy(&m_connectionMutex);          // 连接相关互斥量释放
    pthread_mutex_destroy(&m_sendMessageQueueMutex);    // 发消息互斥量释放
    pthread_mutex_destroy(&m_recyconnqueueMutex);       // 连接回收消息队列相关的互斥量释放
    pthread_mutex_destroy(&m_timequeueMutex);           // 时间处理队列相关的互斥量释放
    sem_destroy(&m_semEventSendQueue);                  // 发消息相关线程互斥量释放
}

// 清理TCP发送消息队列
void CSocket::clearMsgSendQueue()
{
    char *sTmpMempoint;
    CMemory *p_memory = CMemory::GetInstance();

    while (!m_MsgSendQueue.empty())
    {
        sTmpMempoint = m_MsgSendQueue.front();
        m_MsgSendQueue.pop_front();
        p_memeory->FreeMemory(sTmpMempoint);
    }
    
}

// 专门用于读各种配置项
void CSocket::ReadConf()
{
    CConfig *p_config           = CConfig::GetInstance();
    m_worker_connections        = p_config->GetIntDefault("worker_connections", m_worker_connections);              // epoll连接的最大项数
    m_ListenPortCount           = p_config->GetIntDefault("ListenPortCount", m_ListenPortCount);                    // 取得所要监听的端口数量
    m_RecyConnectionWaitTime    = p_config->GetIntDefault("Sock_RecyConnectionWaitTime",m_RecyConnectionWaitTime);  //等待这么些秒后才回收连接

    m_ifkickTimeCount           = p_config->GetIntDefault("Sock_WaitTimeEnable", 0);                                // 是否开启踢人时钟， 1：开启， 0：不开启
    m_iWaitTime                 = p_config->GetIntDefault("Sock_MaxWaitTime", m_iWaitTime);                         // 多少秒检测一次是否心跳超时，只有当socket_waitTimeEnable = 1时，本项才有用
    m_iWaitTime                 = (m_iWaitTime > 5)?m_iWaitTime:5;                                                  // 不建议地域5s，因为心跳无需太频繁
    m_ifTimeOutKick             = p_config->GetIntDefault("Sock_TimeOutKick", 0);                                   // 当时间达到Sock_MaxWaitTime指定的时间时，直接把客户端踢出去，只有当Sock_WaitTimeEnable = 1时，本项才有用

    m_floodAkEnable             = p_config->GetIntDefault("Sock_FloodAttackKickEnable", 0);                         // Flood攻击检测是否开启,1：开启   0：不开启
	m_floodTimeInterval         = p_config->GetIntDefault("Sock_FloodTimeInterval", 100);                           // 表示每次收到数据包的时间间隔是100(毫秒)
	m_floodKickCount            = p_config->GetIntDefault("Sock_FloodKickCounter", 10);                             // 累积多少次踢出此人               

    return;
}

// 监听端口【支持多个端口】,这里遵从nginx官方的命名
// 在创建worker子进程之前就要执行这个函数
bool CSocket::ngx_open_listening_sockets()
{
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
        // 允许一个布尔型选项（参数3），则将参数4指向非零整形数；禁止一个选项参数4指向一个等于零的整形数。对于布尔型选项，参数5应等于sizeof(int)；
        // 对其他选项，参数4指向包含所需选项的整形数或结构，而参数5则为整形数或结构的长度。
        int reuseaddr = 1;  // 1:打开对应的设置项
        if(setsockopt(isock, SOL_SOCKET, SO_REUSEADDR, (const void *) &reuseaddr, sizeof(reuseaddr)) == -1)
        {
            ngx_log_stderr(errno, "CScoket::Initialize()中setsocopt(SO_REUSEADDR)失败，i = %d。", i);
            close(isock);   // 无需理会是否正常执行
            return false;
        }

        // 为处理惊群问题使用reuseport
        int reuseport = 1;
        if (setsockopt(isock, SOL_SOCKET, SO_REUSEPORT, (const void *)&reuseport, sizeof(int)) == -1)   // 端口复用需要内核支持
        {
            //失败就失败吧，失败顶多是惊群，但程序依旧可以正常运行，所以仅仅提示一下即可
            ngx_log_stderr(errno,"CSocket::Initialize()中setsockopt(SO_REUSEPORT)失败",i);
        }
        

        // 设置socket为非阻塞
        if(setnonblocking(isock) == false)
        {
            ngx_log_stderr(errno, "CSocket::Initialize()中setnonblocking()失败,i=%d.",i);
            close(isock);
            return false;
        }

        // 设置本服务器要监听的地址和端口，这样客户端才能连接到该地址和端口并发送数据
        strinfo[0] = 0;
        // C 库函数 int sprintf(char *str, const char *format, ...) 发送格式化输出到 str 所指向的字符串。
        sprintf(strinfo, "ListenPort%d", i);
        iport = p_config->GetIntDefault(strinfo, 10000);
        serv_addr.sin_port = htons((in_port_t)iport);   // in_port_t其实就是uint16_t

        // 绑定服务器地址结构体
        /* 
        * __fd:socket描述字，也就是socket引用
        * myaddr:要绑定给sockfd的协议地址
        * __len:地址的长度
        */
        if(bind(isock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
        {
            ngx_log_stderr(errno, "CSocket::Initialize()中bind()失败,i=%d.",i);
            close(isock);
            return false;
        }

        // 开始监听
        if(listen(isock, NGX_LISTEN_BACKLOG) == -1)
        {
            ngx_log_stderr(errno, "CSocket::Initialize()中listen()失败,i=%d.",i);
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
    //     ngx_log_stderr(errno,"CSocket::setnonblocking()中fcntl(F_GETFL)失败.");
    //     return false;
    // }
    // opts |= O_NONBLOCK; // 把非阻塞标记加到原来的标记上，标记这是一个非租塞套接字，【如何关闭非阻塞呢？opts &= ~O_NONBLOCK;然后再F_SETFL 一下即可】
    // if(fcntl(sockfd, F_SETFL, opts) < 0)
    // {
    //     ngx_log_stderr(errno,"CSocket::setnonblocking()中fcntl(F_SETFL)失败.");
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

// 将一个待发送的数据放入到发送消息队列中
void CSocket::msgSend(char *psendbuf)
{
    CMemory *p_memory = CMemory::GetInstance();

    CLock lock(&m_sendMessageQueueMutex);

    if (m_iSendMsgQueueCount > 50000)
    {
        // 发送队列过大，比如客户端恶意不接收数据，就会导致这个队列越来越大
        // 那么为了服务器安全考虑，干掉一些数据发送，虽然有可能导致客户端出现问题，但是总比服务器不稳定好很多
        m_iDiscardSendPkgCount++;
        p_memory->FreeMemory(psendbuf);
        return;
    }

    // 总体数据并无风险，不会导致服务器奔溃，要看个体数据，找一下恶意者
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)psendbuf;
    lpngx_connection_t p_Conn = pMsgHeader->pConn;
    if (pConn->iSendCount > 400)
    {
        // 该用户收消息太慢【或者干脆不收消息】。积累的该用户的发送队列中的数据条目数过大，认为是恶意用户，直接切断
        ngx_log_stderr(0,"CSocket::msgSend()中发现某用户%d积压了大量待发送数据包，切断与他的连接！",p_Conn->fd); 
        m_iDiscardSendPkgCount++;
        p_memory->FreeMemory(psendbuf);
        zdCloseSocketProc(p_Conn);      // 直接关闭
        return;
    }
    
    ++p_Conn->iSendCount;           // 发送队列中有的数据条目+1
    m_MsgSendQueue.push_back(psendbuf);
    ++m_iSendMsgQueueCount;     // 原子操作

    // 将信号量的值+1， 这样其他卡在sem_wait的就可以走下去
    if (sem_post(&m_semEventSendQueue) == -1)   // 让ServerSendQueueThread()流程走下来干活
    {
        ngx_log_stderr(0,"CSocket::msgSend()sem_post(&m_semEventSendQueue)失败.");  
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
    initconnection();

    // m_connection_n = m_worker_connections;      // 记录当前连接池中的连接总数
    // // 连接池【数组，每个元素时一个对象】
    // m_pconnections = new ngx_connection_t[m_connection_n];      // new是不可能失败，这里不用判断，如果失败，直接报异常会更好
    // // m_pread_events = new ngx_event_t[m_connection_n];
    // // m_pwrite_events = new mgx_event_t[m_connection_n];
    // // for(int i = 0; i < m_connection_n; i++)
    // // {
    // //     m_pconnections[i].instance = 1; // 失效标志位设置为1,
    // // }

    // int i = m_connection_n;             // 连接池中的连接数
    // lpngx_connection_t next = NULL;
    // lpngx_connection_t c = m_pconnections;  // 连接池数组的首地址

    // // 这个do while循环就是在初始化连接池，并将连接池数组元素通过next指针绑定到一起，形成链表
    // do
    // {
    //     i--;                    // 注意i是数字末尾，从最后遍历，i递减至数组的首个元素

    //     // 从尾部往头部走----------------------
    //     c[i].data = next;       // 设置连接对象的next指针，注意第一次循环时next = NULL
    //     c[i].fd = -1;           // 初始化连接，无socket和该连接池中的连接【对象】绑定
    //     c[i].instance = 1;      // 失效标志位设置为1,【失效】
    //     c[i].iCurrsequence = 0; // 当前序号统一从 0 开始

    //     // -----------------------------------

    //     next = &c[i];           // next指针向前移

    // } while (i);    // 循环直到 i 为 0 .即循环到数组首地址
    // // 注意这里：当这个循环执行完毕后，next的指向现在是指向这个链表的表头

    // m_pfree_connections = next;         // 设置空闲连接链表头指针，因为现在next指向c[0]，注意现在整个链表都是空的
    // m_free_connection_n = m_connection_n;   // 空闲连接链表的长度，因为现在整个链表都是空的，所以这两个参数相等

    // 3）遍历所有监听socket【监听端口】，我们为每个监听socket增加一个 连接池 中的连接。说白了，就是让一个socket和一个内存绑定，以方便记录该socket相关的数据，状态等
    std::vector<lpngx_listening_t>::iterator pos;
    for(pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        lpngx_connection_t p_Conn = ngx_get_connection((*pos)->fd);     // 从连接池中获取一个空闲的连接对象
        if(p_Conn == NULL)
        {
            // 这是致命问题，刚开始怎么可能连接池就为空呢？
            ngx_log_stderr(errno,"CSocket::ngx_epoll_init()中ngx_get_connection()失败.");
            exit(2); // 致命问题，直接退，交给系统处理释放
        }
        p_Conn->listening = (*pos);      // 连接对象 和 监听对象关联，方便通过连接对象找到监听对象
        (*pos)->connection = p_Conn;     // 监听对象 和 连接对象关联，方便通过监听对象找到连接对象

        // rev->accept = 1;         // 监听端口必须设置accept标志为1

        // 对于监听端口的读事件设置处理方法，因为监听端口是用来等待对象连接的发送三次握手的，所以监听端口关心的就是【读事件】
        p_Conn->rhandler = &CSocket::ngx_event_accept;

        // 往监听socket上增加监听事件，从而开始让监听端口履行其职责【如果不加这行，虽然端口能连接上，但是不会触发ngx_epoll_process_events()里面的epoll_wait()往下走
        // ngx_epoll_add_event参数
        // (*pos)->fd,              socket句柄
        // 1, 0,                    读， 写 【只关心读事件，所以参数2：readevent = 1，而参数3：writeevent = 0;】
        // 0,                       其他事件补充标记
        // EPOLL_CTL_ADD,           事件类型【增加，还有其他事件 MOV(修改),DEL(删除)】
        // c                        连接池中的连接
        // if(ngx_epoll_add_event((*pos)->fd, 1, 0, 0, EPOLL_CTL_ADD, c) == -1)
        // (*pos)->fd,              socket句柄
        // EPOLL_CTL_ADD,           事件类型，这里是增加
        // EPOLLIN|EPOLLRDHUP,      标志，这里代表要增加的标志，EPOLLIN:可读，EPOLLRDHUP:TCP连接的远端关闭或者半关闭
        // 0,                       对于事件类型为增加的，不需要这个参数
        // p_Conn                   连接池中的连接
        if(ngx_epoll_oper_event((*pos)->fd, EPOLL_CTL_ADD, EPOLLIN|EPOLLRDHUP, 0, p_Conn) == -1)
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


// 废弃写法
// epoll增加事件，可能被ngx_epoll_init()等函数调用
// fd:句柄，一个socket
// readevent：表示是否是一个读事件，0是，1不是
// writeevent：表示是否是一个写事件，0是，1不是
// otherflag：其他需要额外补充的标记
// eventtype：事件类型，一帮用的就是系统的枚举值， 增加，删除，修改等
// c：对应的连接池中的连接的指针
// 返回值：成功返回1，失败返回-1
// int CSocket::ngx_epoll_add_event(int fd, int readevent, int writeevent, uint32_t otherflag, uint32_t eventtyple, lpngx_connection_t c)
// {
//     struct epoll_event ev;
//     memset(&ev, 0, sizeof(ev));

//     if(readevent == 1)
//     {
//         // 读事件，这里发现官方nginx没有使用EPOLLERR，所以这里我也不使用
//         ev.events = EPOLLIN|EPOLLRDHUP; // EPOLLIN读事件，也就是read ready【客户端三次握手连接进来，也属于一种可读事件】 EPOLLRDHUP 客户端关闭连接，断联
//                                         // 这里似乎不用加EPOLLERR,只用EPOLLRDHUP即可，EPOLLERR/EPOLLRDHUP实际上是通过触发读写事件进程读写操作 recv write来检测连接异常

//         // ev.events |= (ev.events | EPOLLET); // 只支持费阻塞socket的高速模式【ET：边缘触发】，就拿accept来说，如果加这个EPOLLET，则客户端连入时，epoll_wait()只会返回一次该事件
//                                             // 如果用的是EPOLLLT【LT：水平触发】，则客户端连入时，epoll_wait()就会被触发多次，一直到用accept()来处理

//         //https://blog.csdn.net/q576709166/article/details/8649911
//         // 关于EPOLLERR的一些说法
//         // 1）对端正常关闭（程序里close(),shell下kill或者ctrl+c），触发EPOLLIN和EPOLLRDHUP，但是不触发EPOLLERR 和 EPOLLHUP
//         // 2）EPOLLRDHUP    这个好像系统检测不到，可以使用EPOLLIN，read返回0，删除掉事件，关闭close(fd)；如果有EPOLLRDHUP，检测他就可以知道是对方关闭，否者就用上面的方法
//         // 3）client端close()连接，server会报某个sockfd可读，即epollin来临，然后recv一下，如果返回0在调用epoll_stl中的EPOLL_CTL_DEL,同时关闭close(sockfd);
//         // 有些系统会收到一个EPOLLRDHUP，当然检测这个是最好不过，只可惜是有些系统。所以使用上面的方法最保险，如果能加上对EPOLLRDHUP的处理那就是万能的了
//         // 4）EPOLLERR  只有采取动作时，才能知道是否对方异常，即如果对方突然断掉，那是不可能有此事件发生的。只有自己采取动作（当然自己此时也不知道）read,write时，出EPOLLERR错，说明对方已经异常断开
//         // 5）EPOLLERR  是服务器这边出错（自己出错能检测到）
//         // 6）给已经关闭的socket写时，会发生EPOLLERR，也就是说，只有在采取行动（比如：读一个已经关闭的socket，或者写一个已经关闭的socket）的时候，才知道对方是否已经关闭了
//         // 这个时候，如果对方异常关闭了，则会出现EPOLLERR，出现Error把对方DEL掉，close就可以

//     }
//     else
//     {
//         // 其他事件类型
//     }

//     if(otherflag != 0)
//     {
//         ev.events |= otherflag;
//     }

//     // 以下代码出自官方nginx，因为指针的最后一位【二进制位】肯定不是1，所以，和c->instance做 | 运算；到时候通过一些编码，即可以取得C的真实地址，又可以把此时此刻的c->instance值取到
//     // 比如c是个地址，可能的值是 0x00af0578, 对应的二进制是‭101011110000010101111000，而 | 1 后，是0x00af0579
//     ev.data.ptr = (void *)((uintptr_t)c | c->instance);     // 把对象弄进去，后续来事件时，用epoll_wait()后，这个对象就能被取出来
//                                                             // 但同时把一个标志位【不是0就是1】弄进去

//     if(epoll_ctl(m_epollhandle, eventtype,fd,&ev) == -1)
//     {
//         ngx_log_stderr(errno, "CSocket::ngx_epoll_add_event()中epoll_ctl(%d,%d,%d,%u,%u)失败.",fd,readevent,writeevent,otherflag,eventtype);
//         // exit(2); // 致命问题，直接退出，资源交由系统释放，后来发现不能直接退
//         return -1;
//     }

//     return 1;

// }


// 对于epoll事件的具体操作
// 返回值：成功返回1，失败返回-1
/**
 * 参数列表：
 * int fd                           句柄，一个socket
 * uint32_t eventtype               事件类型，一般是EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLL_CTL_DEL 说白了就是操作epoll红黑树的节点（增加，修改，删除）
 * uint32_t flag                    标志，具体含义取决于eventtype
 * int bcaction                     补充动作，用于补充flag标记的不足 ： 0 增加 ； 1  去掉
 * lpngx_connection_t pConn         一个指针【实际上是一个连接】EPOLL_CTL_ADD的时候增加到红黑树中去，将来在epoll_wait的时候能取出来
 * */
int CSocket::ngx_epoll_oper_event(int fd, uint32_t eventtype, uint32_t flag, int bcaction, lpngx_connection_t pConn)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    if (eventype == EPOLL_CTL_ADD)  // 往红黑树中加节点
    {
        // 红黑树中从无到有增加节点
        // ev.data.ptr = (void*)pConn;
        ev.events = flag;               // 既然是增加节点，则不管原来是什么标记
        pConn->events = flag;           // 这个连接本身也记录这个标记
    }
    else if (eventtype == EPOLL_CTL_MOD)
    {
        // 节点已经在红黑树中，修改节点的事件信息
    }
    else
    {
        // 删除红黑树中节点，目前没这个需求，所以将来再拓展
        return 1;          // 先直接返回1表示成功
    }

    // 原来的理解中，绑定ptr这个事，只在EPOLL_CTL_ADD的时候做一次即可，但是发现EPOLL_CTL_MOD似乎会破坏掉.data.ptr，因此不管是EPOLL_CTL_ADD还是EPOLL_CTL_MOD都给进去
    // linux内核源码中SYSCALL_DEFINE4(epoll_ctl, int, epfd, int, op, int, fd, struct epoll_event __user *, event)，它这里对于epoll_ctl_mod的处理会重新覆盖一遍，所以在epoll_ctl_mod的时候，不可以不传ptr
    // copy_from_user(&epds, event, sizeof(struct epoll_event)))，感觉这个内核处理这个事情太粗暴了
    ev.data.ptr = (void *)pConn;

    if (epoll_ctl(m_epollhandle, eventtype, fd, &ev) == -1)
    {
        ngx_log_stderr(errno,"CSocket::ngx_epoll_oper_event()中epoll_ctl(%d,%ud,%ud,%d)失败.",fd,eventtype,flag,bcaction);    
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
            ngx_log_error_core(NGX_LOG_INFO,errno,"CSocket::ngx_epoll_process_events()中epoll_wait()失败!");
            return 1;   // 正常返回
        }
        else
        {
            // 这里被认为应该是有问题的，记录日志
            ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocket::ngx_epoll_process_events()中epoll_wait()失败!");
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
        ngx_log_error_core(NGX_LOG_ALERT,0,"CSocket::ngx_epoll_process_events()中epoll_wait()没超时却没返回任何事件!"); 
        return 0; //非正常返回 
    }

    // 会惊群，一个telnet上来，4个worker进程都会被惊动，都执行下面这个
    // ngx_log_stderr(errno,"惊群测试1:%d",events); 

    // 走到这里，就属于有事件收到了
    lpngx_connection_t      c;
    // uintptr_t               instance;
    uint32_t                revents;
    for(int i = 0; i < events; ++i)     // 遍历本次epoll_wait返回的所有事件，注意，events才是返回的实际事件数量
    {
        c = (lpngx_connection_t)(m_events[i].data.ptr);         // ngx_epoll_add_event()给进去的，这里就能取出来

        // instance = (uintptr_t)c & 1;                            // 将地址的最后一位取出来，用instance变量进行标识，见：ngx_epoll_add_event函数。该值当时是随着连接池中的连接一起给传进来的
        // c = (lpngx_connection_t)((uintptr_t) c & (uintptr_t) ~1);   // 最后一位去掉，得到真正的c地址

        // // 仔细分析一下官方nginx这个判断
        // // 一个套接字，当关联一个连接池中的连接【对象】时，这个套接字值是要给到c->fd的
        // // 那么什么时候这个c->fd会变成-1呢？关闭连接时，这个fd会被设置为-1,具体哪行代码设置的-1后续再看，但是这里应该不是ngx_free_connection()函数设置的-1
        // if(c->fd == -1)
        // {
        //     // 比如我们使用epoll_wait取得三个事件，处理第一个事件时，因为业务需要，我们把这个链接关闭，那么我们应该会把c->fd设置为-1；
        //     // 第二个事件照常处理
        //     // 第三个事件：假如这个第三个事件也跟第一个事件对应的是同一个连接，那么这个条件就会成立，那么这种事件就属于过期事件，不应该处理

        //     // 这里可以增加个日志，
        //     ngx_log_error_core(NGX_LOG_DEBUG,0,"CSocket::ngx_epoll_process_events()中遇到了fd=-1的过期事件:%p.",c); 
        //     continue; //这种事件就不处理即可
        // }

        // if(c->instance != instance)
        // {
        //     // 什么时候这个条件成立呢？【换种问法：instance标志为什么可以判断事件是否过期？】
        //     // 比如我们使用epoll_wait取的第单个事件，处理第一个事件时，因为业务需要，我们把这个链接关闭【麻烦就麻烦在这个连接被服务器关闭上了】，但是恰好第三个事件也和这个连接有关；
        //     // 因为第一个事件就把socket连接关闭了，显然第三个事件我们是我们不应该处理的【因为这是一个过期事件】，若处理，肯定会导致错误。
        //     // 那我们把上述c->fd设置为-1,可以解决这个问题吗？能解决问题的一部分，但是另外一部分不能解决，不能解决的部分描述如下：【这种情况应该很少见到】
        //     // 1）处理第一个事件时，一位业务需要，我们把这个连接【假设套接字为50】关闭，同时设置c->fd = -1；并且调用ngx_free_connection将这个连接归还给连接池
        //     // 2）处理第二个事件，恰好第二个事件时建立新连接事件，调用ngx_get_connection从连接池中取出的连接非常可能是刚刚释放的第一个事件对应的连接池中的连接
        //     // 3）又因为 （1）中套接字50被释放了，所以会被操作系统拿过来进行复用，复用给了（2）【一般这么快被复用也是很少见了】
        //     // 4）当处理第三个事件时，第三个事件其实是已经过期的，应该不处理，那么怎么判断第三个事件是过期的呢？
        //     //      【假设现在处理的是第三个事件，此时这个连接池中的该连接实际上已经被作用于第二个事件对应的socket了】
        //     //      依靠instance标志位能够解决这个问题，当调用ngx_get_connection从连接池中获取一个新连接时，我们把instance标志位置反，所以这个条件如果不成立，说明这个连接已经被挪作他用了

        //     // 总结：
        //     // 如果收到了若干个事件，其中连接关闭也搞了多次，导致这个instance标志位被取反两次，那么造成的结果就是：还是有可能遇到某些过期事件没有被发现【这里也就没有被continue】，
        //     // 然后照旧被单做新事件处理了，
        //     // 如果是这样，那就只能被照旧处理了，可能会照成偶尔某个连接被误关闭？但是整体服务器程序运行应该是平稳的，问题不大。这种漏网之鱼而被单做没过期来的过期事件理论上是极少发生的

        //     ngx_log_error_core(NGX_LOG_DEBUG,0,"CSocket::ngx_epoll_process_events()中遇到了instance值改变的过期事件:%p.",c); 
        //     continue;   // 这种事件不处理即可
        // }

        // 能走到这里，说明这些事件我们都认为没有过期，就开始正常处理
        revents = m_events[i].events;   // 取出事件类型
        // if(revents & (EPOLLERR | EPOLLHUP))     // 例如对方close掉套接字，这里会感应到，【换句话说，就是如果发生了错误或者客户端断联】
        // {
        //     // 这里加上读写标记，方便后续代码处理
        //     revents |= EPOLLIN | EPOLLOUT;  // EPOLLIN: 表示对应的连接上有数据可以读出，（tcp连接的远端主动关闭连接，也相当于可读事件，因为本服务器是要处理发送过来的FIN包）
        //                                     // EPOLLOUT: 表示对应的连接上可以写入数据发送【写准备好】

        //     //ngx_log_stderr(errno,"2222222222222222222222222.");
        // }
        if(revents & EPOLLIN)   // 如果是读事件
        {
            // 一个客户端新连入，这个就会成立
            // 已连接发送数据来，这个也成立
            // c->r_ready = 1;              // 标记可读。【从连接池中拿出一个连接时，这个连接的所有成员都是0】
            (this->*(c->rhandler))(c);      // 注意括号的运用，来正确的设置优先级，防止编译出错；如果是个新客户连入
                                            // 如果新连接进入，这里执行的应该是CSocket::ngx_event_accept(c)
                                            // 如果是已经连入，发送数据到这里，则这里执行的应该是CSocket::ngx_read_request_handler
            // 为什么可以这样掉用呢？看该函数指针定义方法（他是一个成员函数指针）
            // typedef void (CSocket::*ngx_event_handler_pt)(lpngx_connection_t c);    // 定义成员函数指针
        }

        if(revents & EPOLLOUT) // 如果是写事件【对方关闭连接也触发这个，再研究。。。】注意上面的if (revents & (EPOLLERR|EPOLLHUP)) revents |= EPOLLIN|EPOLLOUT；读写标记都给加上了
        {
            // 。。。等待扩展 客户端关闭时，关闭的时候能够执行到这里，因为上边有if(revents & (EPOLLERR|EPOLLHUP))  revents |= EPOLLIN|EPOLLOUT; 代码
            // ngx_log_stderr(errno,"111111111111111111111111111111.");
            if (revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))   // 客户端关闭，如果服务器端挂着一个写通知事件，则这里这个条件是可能成立的
            {
                // EPOLLERR：对应的连接发生错误                     8       = 1000
                // EPOLLHUP: 对应的连接被挂起                       16      = 0001 0000
                // EPOLLRDHUP: 表示TCP连接的远端关闭或者半关闭连接   8192    = 0010 0000 0000 0000
                // 打印日志查看是否会出现这种情况
                // 8221 = 0010 0000 0001 1101 ： 包括 EPOLLRDHUP, EPOLLHUP, EPOLLERR
                //ngx_log_stderr(errno,"CSocket::ngx_epoll_process_events()中revents&EPOLLOUT成立并且revents & (EPOLLERR|EPOLLHUP|EPOLLRDHUP)成立,event=%ud。",revents); 

                // 我们只有注册了写事件， 但是对端断开时，程序流程才走到这里，注册了事件意味着 iThrowsendCount标记肯定被+1了，这里我们减回来
                --p_Conn->iThrowsendCount;
            }
            else
            {
                (this->*(p_Conn->whandler))(p_Conn);        // 如果有数据没有发送完毕，由系统驱动来发送，则这里执行的应该是 CSocket::ngx_write_request_handler()
            }
            
        }
    }
    return 1;

}

// --------------------------------------------------------------------------------------------------
// 处理发送消息队列的线程
void* CSocket::ServerSendQueueThread(void* threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CSocket *pSocketObj = pThread->_pThis;
    int err;
    std::list<char*>::iterator pos, pos2,posend;

    char *pMsgBuf;
    LPSTRUC_MSG_HEADER  pMsgHeader;
    LPCOMM_PKG_HEADER   pPkgHeader;
    lpngx_connection_t  p_Conn;
    unsigned short      itmp;
    ssize_t             sendsize;

    CMemeory *p_memory = CMemory::GetInstance();

    while (g_stopEvent == 0)        // 不退出
    {
        // 如果信号量值 > 0，则-1并且走下去，否者卡在这里等待【为了让信号量值+1，可以在其他线程调用sem_post达到，实际上在CSocket::msgSend()调用的sem_post就达到了让这里sem_wait走下去的目的】
        // 如果被某个信号中断，sem_wait也可能过早的返回，错误为EINTR
        // 整个程序退出之前，也要sem_post一下，以确保如果本线程卡在sem_wait()，也能走下去从而让本线程成功返回
        if (sem_wait(&pSocketObj->m_semEventSendQueue) == -1)
        {
            // 失败？及时报告打印
            if (errno != EINTR)     // 这个这里不算一个错误【当阻塞于某个慢系统调用的一个进程捕获某个信号且相应的信号处理函数返回时，该系统调用可能返回一个EINTR错误】
            {
                ngx_log_stderr(errno,"CSocket::ServerSendQueueThread()中sem_wait(&pSocketObj->m_semEventSendQueue)失败.");   
            }
            
        }

        // 一般走到这里表示需要处理数据收发了
        if (g_stopEvent != 0)       // 要求整个进程退出
        {
            break;
        }

        if (pSocketObj->m_iSendMsgQueueCount > 0)       // 原子的
        {
            err = pthread_mutex_lock(&pSocketObj->m_sendMessageQueueMutex);     // 因为我们这里要操作发送消息队列m_MsgSendQueue，所以这里要临界
            if (err != 0)
            {
                ngx_log_stderr(err,"CSocket::ServerSendQueueThread()中pthread_mutex_lock()失败，返回的错误码为%d!",err);
            }

            pos = pSocketObj->m_MsgSendQueue.begin();
            posend = pSocketObj->m_MsgSendQueue.end();

            while (pos != posend)
            {
                pMsgBuf = (*pos);                                                               // 拿到的每个消息都是 消息头 + 包头 + 包体 【但是要注意，我们是不发消息头给客户端的】
                pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;                                       // 指向消息头
                pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf + pSocketObj->m_iLenMsgHeader);        // 指向包头
                p_Conn = pMsgHeader->pConn;

                // 包过期，因为如果这个连接被回收，比如在ngx_close_connection()，inRecyConnectQueue()中都会自增iCurrsequeue
                // 而且这里没有必要针对 本连接 来用m_connectionMutex临界，只要下面条件成立，肯定是客户端连接已经断开，要发送的数据肯定不再需要发送了
                if (p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)     // 包过期
                {
                    // 本包中保存的序列号与p_Conn【连接池中连接】中实际的序列号已经不同，丢弃此消息，小心处理该消息的删除
                    pos2 = pos;
                    pos++;
                    pSocketObj->m_MsgSendQueue.erase(pos2);
                    --pSocketObj->m_iSendMsgQueueCount;     // 发送消息队列容量-1
                    p_memory->FreeMemory(pMsgBuf);
                    continue;
                }

                if (p_Conn->iThrowsendCount > 0)
                {
                    // 靠系统驱动来发送消息，所以这里不能再发送
                    pos++;
                    continue;
                }

                --p_Conn->iSendCount;           // 发送队列中有的数据条目数-1

                // 走到这里，可以发送消息，一些必须的信息记录，在发送的东西也要从消息队列中干掉
                p_Conn->psendMemPointer = pMsgBuf;              // 发送后释放用
                pos2 = pos;
                pos++;
                pSocketObj->m_MsgSendQueue.erase(pos2);
                --pSocketObj->m_iSendMsgQueueCount;             // 要发送消息队列容量-1
                p_Conn->psendbuf = (char *)pPkgHeader;          // 要发送的数据的缓冲区指针
                itmp = ntohs(pPkgHeader->pkgLen);               // 包头 + 包体  长度
                p_Conn->isendlen = itmp;                        // 要发送多少数据
                
                // 这里是重点，我们采用epoll的水平触发模式，能走到这里的，应该都是还没有投递写事件到epoll中
                // epoll水平触发发送数据的改进方案：
                // 开始不把socket写事件通知加入到epoll中，当我需要写数据的时候，直接调用write/send发送数据
                // 如果返回了EAGAIN【发送缓冲区满了，需要等待可写事件才能继续往缓冲区里写数据】，此时，我再把写事件加入到epoll中
                // 此时, 就变成了在epoll驱动下写数据，全部数据发送完毕之后，在把写事件通知从epoll中移除
                // 优点：数据不多的时候，可以避免epoll的写事件的增加/删除，提高了程序的执行效率
                // （1）直接调用write或者send去发送数据
                // ngx_log_stderr(errno,"即将发送数据%ud。",p_Conn->isendlen);

                sendsize = pSocketObj->sendproc(p_Conn, p_Conn->psendbuf, p_Conn->isendlen);    // 注意参数
                if (sendsize > 0)
                {
                    // 成功发送出去了数据，一下就发出去这样很顺利
                    if (sendsize == p_Conn->isendLen)
                    {
                        // 成功发送的和要求发送的数据相等，说明全部发送成功 发送缓冲区 【数据全部发送完毕】
                        p_memory->FreeMemory(p_Conn->psendMemPointer);      // 释放内存
                        p_Conn->psendMemPointer = NULL;
                        p_Conn->iThrowsendCount = 0;                        // 其实这里可以没有，因为此时此刻这个东西就是0
                        // ngx_log_stderr(0,"CSockett::ServerSendQueueThread()中数据发送完毕，很好。"); //做个提示，上线时可以干掉
                    }
                    else
                    {
                        // 没有发送完毕（EAGAIN），数据只发出去了一部分，但是肯定是因为发送缓冲区满了,那么
                        // 发送了到了那里？剩余多少？记录下来，方便下次sendproc()时调用
                        p_Conn->psendbuf = p_Conn->psendbuf + sendsize;
                        p_Conn->isendlen = p_Conn->isendlen - sendsize;

                        // 因为发送缓冲区满了，所以现在要依赖系统通知来发送数据了
                        ++p_Conn->iThrowsendCount;                      // 标记发送缓冲区满了，需要通过epoll事件驱动消息的继续发送【原子+1，且不可写成p_Conn->iThrowsendCount = p_Conn->iThrowsendCount +1 ，这种写法不是原子+1】
                        if (pSocketObj->ngx_epoll_oper_event(p_Conn->fd,            // scoket句柄
                                                            EPOLL_CTL_MOD,          // 事件类型，这里是增加【因为我这里打算增加个写通知】
                                                            EPOLLOUT,               // 标记，这里代表要增加的标志，EPOLLOUT：可写【可写的时候通知我】
                                                            0,                      // 对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数，0 ： 增加，1：去掉，2：完全覆盖
                                                            p_Conn                  // 连接池中的连接
                                                            ) == -1)
                        {
                            //有这情况发生？这可比较麻烦，不过先do nothing
                            ngx_log_stderr(errno,"CSocket::ServerSendQueueThread()中ngx_epoll_add_event()失败.");
                        }
                        // ngx_log_stderr(errno,"CSocket::ServerSendQueueThread()中数据没发送完毕【发送缓冲区满】，整个要发送%d，实际发送了%d。",p_Conn->isendlen,sendsize);
                    }

                    continue;       // 继续处理其他消息
                    
                }

                // 能走到这里，应该是有点问题的
                if (sendsize == 0)
                {
                    // 发送0个字节，首先因为我发送的内容不是0个字节的
                    // 然后如果发送缓冲区满则返回的应该是-1，而错误码应该是EAGAIN，所以我综合认为，这个情况我就把这个发送的包丢弃了【按对端关闭了socket处理】
                    // 这个打印一下日志，观察一下是否真的会有这种现象发生
                    // ngx_log_stderr(errno,"CSocket::ServerSendQueueThread()中sendproc()居然返回0？"); //如果对方关闭连接出现send=0，那么这个日志可能会常出现，商用时就 应该干掉
                    // 然后这个包干掉，不发送了
                    p_memory->FreeMemory(p_Conn->psendMemPointer);      // 释放内存
                    p_Conn->psendMemPointer = NULL;
                    p_Conn->iThrowsendCount = 0;  //这里其实可以没有，因此此时此刻这东西就是=0的    
                    continue;
                }
                // 能走到这里，继续处理问题
                if (sendsize == -1)
                {
                    // 发送缓冲区已经满了
                    ++p_Conn->iThrowsendCount;      // 标记发送缓冲区已经满了，需要通过epoll事件来驱动消息的继续发送
                    if (pSocketObj->ngx_epoll_oper_event(p_Conn->fd,        // socket句柄
                                                        EPOLL_CTL_MOD,      // 事件类型，这里是增加【因为我准备增加个写通知】
                                                        EPOLLOUT            // 标志，这里代表要增加的标志，EPOLLOUT：可写【可写的时候通知我】                                                   
                                                        0,                  // 对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数 0：增加， 1：去除， 2：完全覆盖
                                                        p_Conn              // 连接池中的连接
                                                        ) == -1)
                    {
                        //有这情况发生？这可比较麻烦，不过先do nothing
                        ngx_log_stderr(errno,"CSocket::ServerSendQueueThread()中ngx_epoll_add_event()_2失败.");
                    }
                    continue;
                }
                // 能走到这里的，应该就是返回值-2了,一般认为对端断开连接了，等待recv（）来做断开socket以及来回收资源
                p_memory->FreeMemory(p_Conn->psendMemPointer);              // 释放内存
                p_Conn->psendMemPointer = NULL;
                p_Conn->iThrowsendCount = 0;  //这行其实可以没有，因此此时此刻这东西就是=0的  
                continue;
                
            }
            err = pthread_mutex_unlock(&pSocketObj->m_sendMessageQueueMutex); 
            if(err != 0)  ngx_log_stderr(err,"CSocket::ServerSendQueueThread()pthread_mutex_unlock()失败，返回的错误码为%d!",err);
            
        }
        
    }
    return (void*)0;
    
}

// 主动关闭一个连接时要做的一些善后处理函数
void CSocket::zdCloseSocketProc(lpngx_connection_t p_Conn)
{
    if (m_ifkickTimeCount == 1)
    {
        DeleteFromTimerQueue(p_Conn);       // 把时间队列中的连接移除
    }

    if (p_Conn->fd != -1)
    {
        close(p_Conn->fd);          // 这个socket关闭，关闭后epoll就会被从红黑树中删除，所以这之后无法收到任何epoll事件
        p_Conn->fd = -1;
    }
    
    if (p_Conn->iThrowsendCount > 0)
    {
        --p_Conn->iThrowsendCount;  // 归0
    }
    
    inRecyConnectQueue(p_Conn);
    return;
}

// 测试是否flood攻击成立，成立则返回true，否者返回false
bool CSocket::TestFlood(lpngx_connection_t pConn)
{
    struct timeval  sCurrTime;          // 当前时间结构
    uint64_t        iCurrTime;          // 当前时间（单位ms)
    bool reco =     false;

    gettimeofday(&sCurrTime, NULL);     // 取得当前时间
    iCurrTime = (sCurrTime.tv_sec * 1000 + sCurrTime.tv_usec / 1000);   // 毫秒
    if ((iCurrTime - pConn->FloodkickLastTime) < m_floodTimeInterval)      // 两次收到包的时间 < 100毫秒
    {
        // 发包态频繁记录
        pConn->FloodAttackCount++;
        pConn->FloodkickLastTime = iCurrTime;
    }
    else
    {
        // 既然发包不这么频繁，则恢复计数值
        pConn->FloodAttackCount = 0;
        pConn->FloodkickLastTime = iCurrTime;
    }

    if (pConn->FloodAttackCount >= m_floodKickCount)
    {
        // 可以踢人的标志
        reco = true;
    }
    
     return reco
}

// 打印统计信息
void CSocket::printTDInfo()
{
    time_t currtime = time(NULL);
    if ((currtime - m_lastprintTime) > 10)
    {
        // 超过10秒才打印一次
        int tmprmqc = g_threadpool.getRecvMsgQueueCount();  // 收消息队列

        m_lastprintTime = currtime;
        int tmpoLUC = m_onlineUserCount;    //atomic做个中转，直接打印atomic类型报错；
        int tmpsmqc = m_iSendMsgQueueCount; //atomic做个中转，直接打印atomic类型报错；
        ngx_log_stderr(0,"------------------------------------begin--------------------------------------");
        ngx_log_stderr(0,"当前在线人数/总人数(%d/%d)。",tmpoLUC,m_worker_connections);        
        ngx_log_stderr(0,"连接池中空闲连接/总连接/要释放的连接(%d/%d/%d)。",m_freeconnectionList.size(),m_connectionList.size(),m_recyconnectionList.size());
        ngx_log_stderr(0,"当前时间队列大小(%d)。",m_timerQueuemap.size());        
        ngx_log_stderr(0,"当前收消息队列/发消息队列大小分别为(%d/%d)，丢弃的待发送数据包数量为%d。",tmprmqc,tmpsmqc,m_iDiscardSendPkgCount);        
        if( tmprmqc > 100000)
        {
            //接收队列过大，报一下，这个属于应该 引起警觉的，考虑限速等等手段
            ngx_log_stderr(0,"接收队列条目数量过大(%d)，要考虑限速或者增加处理线程数量了！！！！！！",tmprmqc);
        }
        ngx_log_stderr(0,"-------------------------------------end---------------------------------------");
    }
    return;
}
