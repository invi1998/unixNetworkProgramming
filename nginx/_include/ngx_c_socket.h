
#ifndef __NGX_SOCKET_H__
#define __NGX_SOCKET_H__

#include <vector>      //vector
#include <sys/epoll.h> //epoll
#include <sys/socket.h>

// 一些宏定义-----------------------------------------------
#define NGX_LISTEN_BACKLOG  511      // 已完成链接队列，nginx官方给511
#define NGX_MAX_EVENTS      512     // epoll_wait一次最多接收这么多个事件nginx中缺省是512

typedef struct ngx_listening_s  ngx_listening_t, *lpngx_listening_t;
typedef struct ngx_connection_s ngx_connection_t, *lpngx_connection_t;
typedef class CSocket           CSocket;

typedef void (CSocket::*ngx_event_handler_pt)(lpngx_connection_t c);    // 定义成员函数指针

// 一些专门用于结构定义放在这里
typedef struct ngx_listening_s  // 和监听端口有关的结构
{
    int             port;       // 监听的端口号
    int             fd;         // 套接字句柄 socket
}ngx_listening_t, *lpngx_listening_t;

// 以下三个结构是非常重要
// 1）该结构表示一个TCP连接【客户端主动发起，nginx服务器被动接收的TCP连接】
struct ngx_connection_s
{
    int                         fd;                 // 套接字句柄socket
    lpngx_listening_t           listening;          // 如果这个链接被分配给了一个监听套接字，那么这个里面就指向监听套接字对应的那个lpngx_listening_t的内存首地址
    
    // ---------------------------*********************
    unsigned                    instance:1;         // 【位域】失效标志位：0 有效， 1 失效 【这个是官方nginx中就有的，具体作用在 ngx_epoll_process_events()中详解】
    uint64_t                    iCurrsequence;      // 引入一个序号，每次分配出去时+1，这种方法也有可能在一定程度上检测错包废包，具体用法后续完善
    struct    sockaddr          s_sockaddr;         // 保存对方地址信息
    // char                        addr_text[100];     // 地址的文本信息，100足够，一般其实如果是IPV4地址，255.255.255.255，其实只需要20个字节就足够

    // 和读有关的标志----------------------------------------
    // uint8_t                     r_ready;            // 读准备好标记
    uint8_t                     w_ready;            // 写准备好标记

    ngx_event_handler_pt        rhandler;           // 读事件的相关处理方法
    ngx_event_handler_pt        whandler;           // 写事件的相关处理方法


    // ------------------------------------------
    lpngx_connection_t          data;               // 这个是一个指针【等价于传统链表里的next成员：后继指针】，用于指向下一个本类型对象，用于把空闲的连接池对象串起来构成一个单向的链表，方便取用



}

// 每个TCP连接至少需要一个读事件和一个写事件，所以定义事件结构
// typedef struct  ngx_event_s
// {

// }ngx_event_t, *lpngx_event_t;

// -----------------------------**********************************-----------------------------------
// socket相关类
class CSocket
{
    public:
        CSocket();                                          // 构造函数
        virtual ~CSocket();                                 // 虚析构

    public:
        virtual bool Initialize();                          // 初始化函数

    private:
        bool ngx_open_listening_sockets();                  // 监听必须的端口【支持多个端口】
        void ngx_close_listening_sockets();                 // 关闭监听套接字
        bool setnonblocking(int sockfd);                    // 设置非阻塞套接字

    private:
        int                             m_ListenPortCount;      // 监听的端口数量
        std::vector<lpngx_listening_t>  m_ListenSocketList;     // 监听套接字队列
};

#endif