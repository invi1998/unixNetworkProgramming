//和网络以及逻辑处理 有关的函数放这里

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
#include <pthread.h>   //多线程

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
//#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_crc32.h"
#include "ngx_c_slogic.h"  
#include "ngx_logiccomm.h"
#include "ngx_c_lockmutex.h"

// 定义成员函数指针
typedef bool (CLogicSocket::*handler)(  lpngx_connection_t pConn,           // 连接池中连接的指针
                                        LPSTRUC_MSG_HEADER pMsgHeader,      // 消息头指针
                                        char *pPkgBody,                     // 包体指针
                                        unsigned short iBodyLength);        // 包体长度

// 用来保存 成员函数指针 的数组
static const handler statusHandler[] =
{
    // 数组前5个元素，保留，以备将来增加一些基本服务器功能
    &CLogicSocket::_HandlePing,                             // 【0】：心跳包的实现
    NULL,                                                   // 【1】：下标从1开始
    NULL,                                                   // 【2】：下标从2开始
    NULL,                                                   // 【3】：下标从3开始
    NULL,                                                   // 【4】：下标从4开始

    // 开始处理具体的业务逻辑
    &CLogicSocket::_HandleRegister,                         // 【5】：实现具体的注册功能
    &CLogicSocket::_HandleLogIn,                            // 【6】：实现登录功能
    // 其他待拓展
};

#define AUTH_TOTAL_COMMANDS sizeof(statusHandler)/sizeof(handler) //整个命令有多少个，编译时即可知道

// 构造函数
CLogicSocket::CLogicSocket()
{

}

// 析构函数
CLogicSocket::~CLogicSocket()
{

}

// 初始化函数【fork()子进程之前做的事】
// 成功返回true，失败返回false
bool CLogicSocket::Initialize()
{
    // 做一些和本类相关的初始化工作
    // 。。。日后可根据需要进行拓展

    bool bParentInit = CSocket::Initialize();       // 调用父类的同名函数
    return bParentInit;

}


// 处理收到的数据包
// pMsgBuf：消息头 + 包体：
void CLogicSocket::threadRecvProcFunc(char *pMsgBuf)
{
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;                        // 消息头
    LPCOMM_PKG_HEADER  pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf + m_iLenMsgHeader);     // 包头
    void    *pPkgBody = NULL;                                                           // 指向包体的指针
    unsigned short pkglen = ntohs(pPkgHeader->pkgLen);                                  // 客户端指明的包宽度【包头+包体】

    if (m_iLenPkgHeader == pkg_len)
    {
        // 没有包头，只有包体
        if (pPkgHeader->crc32 != 0) // 只有包头的crc值给0
        {
            return;     // crc错误，直接丢弃
        }
        
        pPkgBody = NULL;
    }
    else
    {
        // 有包体，走到这里
        pPkgHeader->crc32 = ntohl(pPkgHeader->crc32);                   // 针对4字节的数据，网络序转为主机序
        pPkgBody = (void *)(pMsgBuf + m_iLenMsgHeader + m_iLenPkgHeader);   // 跳过消息头 以及 包头，指向包体

        // 计算crc值判断包的完整性
        int calccrc = CCRC32::GetInstance()->Get_CRC((unsigned char *)pPkgBody, pkglen - m_iLenPkgHeader);  // 计算纯包体的crc值
        if (calccrc != pPkgHeader->crc32)   // 服务器端根据包体计算crc值，和客户端传递过来的包头中的crc32值做比较
        {
            ngx_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中CRC错误，丢弃数据!");    //正式代码中可以干掉这个信息
			return; //crc错，直接丢弃
        }
        
    }

    // 包crc校验OK才能走到这里
    unsigned short imsgCode = ntohs(pPkgHeader->msgCode);       // 消息代码取出来
    lpngx_connection_t p_Conn = pMsgHeader->pConn;              // 消息头中附带的连接池中的连接的指针

    // 这里需要做一些判断
    // （1）如果从收到客户端发送来的包，到服务器释放一个线程池中的线程的过程中，客户端断开了，那么显然，这种收到的包我们就不用做处理了
    if (p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)     // 该连接池中连接被其他tcp连接【其他socket】占用了。这说明原来的客户端和本服务器之间的连接断了，这种包直接丢弃不用做处理
    {
        return;
    }

    // （2）判断消息码是正确的，防止客户端恶意侵害服务器，发送一个不在服务器处理范围内的消息码
    if (imsgCode >= AUTH_TOTAL_COMMANDS)    // 无符号数不可能<0
    {
        ngx_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码不对!",imsgCode); //这种有恶意倾向或者错误倾向的包，希望打印出来看看是谁干的
        return; //丢弃不理这种包【恶意包或者错误包】
    }
    
    // 能走到这里的，说明包没过期，不恶意，继续判断是否有对应的处理函数
    // （3）有对应的消息处理函数
    if (statusHandler[imsgCode] == NULL)    // 这种利用imsgCode的方式可以使查找要执行的成员函数效率特别高
    {
        ngx_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码找不到对应的处理函数!",imsgCode); //这种有恶意倾向或者错误倾向的包，希望打印出来看看是谁干的
        return;  //没有相关的处理函数
    }
    
    // 一切正确，可以放心处理
    // （4）调用消息码对应的成员函数来处理
    (this->*statusHandler[imsgCode])(p_Conn, pMsgHeader, (char *)pPkgBody, pkglen - m_iLenPkgHeader);
    return;
    
}

//----------------------------------------------------------------------------------------------------------
//处理各种业务逻辑
bool CLogicSocket::_HandleRegister(lpngx_connection_t pConn,LPSTRUC_MSG_HEADER pMsgHeader,char *pPkgBody,unsigned short iBodyLength)
{
    // ngx_log_stderr(0,"执行了CLogicSocket::_HandleRegister()!");

    // （1）首先判断包体的合法性
    if (pPkgBody == NULL)       // 具体看客户端服务器约定，如果约定这个命令【msgCode】必须带包体，那么如果不带包体，就认为是恶意包，直接不处理
    {
        return false;
    }

    int iRecvLen = sizeof(STRUCT_REGISTER);
    if (iRecvLen != iBodyLength)    // 发送过来的数据结构大小不对，认为是恶意包，直接不处理
    {
        return false;
    }

    // （2）对于同一个用户，可能同时发送过来多个请求，造成多个线程同时为该用户服务，比如以网游为例，用户要在商店中买A物品，又买B物品，而用户的钱 只够买A或者B，不够同时买A和B呢？
    // 那如果用户发送购买命令过来买了一次A，又买了一次B，如果是两个线程来执行同一个用户的这两次不同的购买命令，很可能造成这个用户购买成功了 A，又购买成功了 B
    // 所以为了稳妥起见，针对某个用户的命令，我们一般都要进行互斥，需要增加临界的变量在ngx_connection_s结构中
    Clock lock(&pConn->logicPorcMutex);         // 凡是和本用户有关的访问都要互斥

    // （3）取得了整个发送过来的数据
    LPSTRUCT_REGISTER p_RecvInfo = (LPSTRUCT_REGISTER)pPkgBody;

    // （4）这里可能要考虑根据业务逻辑，进一步判断收到数据的合法性
    // 当前该用户的状态是否适合收到这个数据等等【其实就是登陆验证】
    // 。。。。。。。。

    // （5）给客户端返回数据时，一般也是返回一个结构，这个结构内容具体由客户端/服务器协商，这里我们就可以给客户端也返回同样的 STRUCT_REGISTER 结构来举例
    // LPSTRUCT_REGISTER pFromPkgHeader = (LPSTRUCT_REGISTER)(((char*)pMsgHeader) + m_iLenMsgHeader);      // 指向收到的包的包头，其中数据后续可能要用到
    LPCOMM_PKG_HEADER pPkgHeader;
    CMemory *p_memory = CMemory::GetInstance();
    CCRC32  *p_crc32  = CCRC32::GetInstance();
    int     iSendLen = sizeof(STRUCT_REGISTER);

    // a) 分配要发送出去的包的内存
    iSendLen = 65000;       // unsigned最大也就是这个值
    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iSendLen, false);   // 准备发送的格式，这里是消息头 + 包头 + 包体
    // b) 填充消息头
    memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader);                     // 消息头直接拷贝到这里
    // c）填充包头
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf + m_iLenMsgHeader);      // 指向包头
    pPkgHeader->msgCode = _CMD_REGISTER;                                // 消息代码，可以统一在ngx_logiccomm.h中定义
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode);                   // 主机序转网络序
    pPkgHeader->pkgLen  = htons(m_iLenPkgHeader + iSendLen);            // 整个包的尺寸【包头+包体】

    // d) 填充包体
    LPSTRUCT_REGISTER p_sendInfo = (LPSTRUCT_REGISTER)(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader);  // 跳过消息头，跳过包头，就是包体
    // 。。。。。这里可以根据需要，填充要发回给客户端的内容，int类型要使用htonl()转，short类型要使用htons转

    // e) 包体内容全部确定好之后，计算包体的crc32值
    pPkgHeader->crc32   = p_crc32->Get_CRC((unsigned char *)p_sendInfo, iSendLen);
    pPkgHeader->crc32   = htonl(pPkgHeader->crc32);

    //f)发送数据包
    msgSend(p_sendbuf);
    //如果时机OK才add_event
    // if(ngx_epoll_add_event(pConn->fd,                 //socket句柄
    //                             0,1,              //读，写 ,这里读为1，表示客户端应该主动给我服务器发送消息，我服务器需要首先收到客户端的消息；
    //                             0,//EPOLLET,      //其他补充标记【EPOLLET(高速模式，边缘触发ET)】
    //                                                   //后续因为实际项目需要，我们采用LT模式【水平触发模式/低速模式】
    //                             EPOLL_CTL_MOD,    //事件类型【增加，还有删除/修改】                                    
    //                             pConn              //连接池中的连接
    //                             ) == -1)
    //                             {
    //                                 //ngx_log_stderr(0,"111111111111!");
    //                             }

   /*
    sleep(100);  //休息这么长时间
    //如果连接回收了，则肯定是iCurrsequence不等了
    if(pMsgHeader->iCurrsequence != pConn->iCurrsequence)
    {
        //应该是不等，因为这个插座已经被回收了
        ngx_log_stderr(0,"插座不等,%L--%L",pMsgHeader->iCurrsequence,pConn->iCurrsequence);
    }
    else
    {
        ngx_log_stderr(0,"插座相等哦,%L--%L",pMsgHeader->iCurrsequence,pConn->iCurrsequence);
    }*/
    
    

    return true;
}


bool CLogicSocket::_HandleLogIn(lpngx_connection_t pConn,LPSTRUC_MSG_HEADER pMsgHeader,char *pPkgBody,unsigned short iBodyLength)
{
    ngx_log_stderr(0,"执行了CLogicSocket::_HandleLogIn()!");
    return true;
}

// 心跳包检测时间到，该去检测心跳包是否超时的事宜，本函数是子类函数，实现具体的判断动作
void CLogicSocket::procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg, time_t cur_time)
{
    CMemory *p_memory = CMemory::GetInstance();

    if (tmpmsg->iCurrsequence == tmpmsg->pConn->iCurrsequence)      // 此连接没断
    {
        lpngx_connection_t p_Conn = tmpmsg->pConn;
        if (m_ifTimeOutKick == 1)   // 能调用到本函数第一个条件肯定成立，所以第一个条件加不加无所谓，主要是第二个条件 if(/*m_ifkickTimeCount == 1 && */m_ifTimeOutKick == 1)
        {
            // 到时间直接踢出去的需求
            zdCloseSocketProc(p_Conn);
        }
        // 超时踢出的判断标准就是 每次检测的时间间隔*3 ，超出这个事件没法送心跳包，就踢【这个可以根据实际自由设定】
        else if ((cur_time - p_Conn->lastPingTime) > (m_iWaitTime*3+10))
        {
            // 踢出 【如果此时此刻该用户正好断线，则这个socket可能立即被后续上来的连接复用，如果真的有人这么倒霉，赶上这个点了，那么可能就会错踢，错踢就错踢了吧，让他重新连接一次】
            ngx_log_stderr(0,"时间到不发心跳包，踢出去!");   //感觉OK
            zdCloseSocketProc(p_Conn);
        }

        p_memory->FreeMemory(tmpmsg);   // 内存要释放
        
    }
    else    // 此连接断开了
    {
        p_memory->FreeMemory(tmpmsg);   // 内存要释放
    }
    return;
}

// 发送没有包体的数据包给客户端
void CLogicSocket::SendNoBodyPkgToClient(LPSTRUC_MSG_HEADER pMsgHeader, unsigned short iMsgCode)
{
    CMemory *p_memory = CMemory::GetInstance();

    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader, false);
    char *p_tmpbuf = p_sendbuf;

    memcpy(p_tmpbuf, pMsgHeader, m_iLenMsgHeader);
    p_tmpbuf += m_iLenMsgHeader;

    LPCOMM_PKG_HEADER pPkgHeader = (LPCOMM_PKG_HEADER)p_tmpbuf;     // 指向的是要发送数据的包头
    pPkgHeader->msgCode = htons(iMsgCode);
    pPkgHeader->pkgLen = htons(m_iLenPkgHeader);
    pPkgHeader->crc32 = 0;
    msgSend(p_sendbuf);
    return;
}


// 接收并处理客户端发送过来的ping包
bool CLogicSocket::_HandlePing(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgBody, unsigned short iBodyLength)
{
    // 心跳包要求没有包体
    if (iBodyLength != 0)   // 有包体则认为是 非法包
    {
        return false;
    }

    CLock lock(&pConn->logicPorcMutex);     // 凡是和本用户有关的访问都考虑使用互斥，以免该用户勇士发送过来两个命令达到各种作弊目的
    pConn->lastPingTime = time(NULL);       // 更新该变量

    // 服务器也发送一个只有包头的数据包给客户端，作为返回数据
    SendNoBodyPkgToClient(pMsgHeader, _CMD_PING);

    ngx_log_stderr(0,"成功收到了心跳包并返回结果！");
    return true;
    
}