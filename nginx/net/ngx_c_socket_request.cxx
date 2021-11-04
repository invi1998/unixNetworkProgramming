
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
#include <pthread.h>   //多线程

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"  //自动释放互斥量的一个类

//来数据时候的处理，当连接上有数据来的时候，本函数会被ngx_epoll_process_events()所调用  ,官方的类似函数为ngx_http_wait_request_handler();
void CSocket::ngx_read_request_handler(lpngx_connection_t pConn)
{
    bool isflood = false;       // 是否flood攻击
    // 收包，注意这里使用的第二个和第三个参数，我这里用的始终都是这俩个参数，所以必须要保证 pConn->precvbuf 指向正确的收包位置，保证c->irecvlen指向正确的收包宽度
    ssize_t reco = recvproc(pConn, pConn->precvbuf, pConn->irecvlen);
    if (reco <= 0)
    {
        return;     // 该处理的上面这个recvproc()函数里处理过了，这里<=直接退出
    }

    // 走到这里，说明成功收到了一些字节（>0 ）,就要开始判断收到多少数据了
    if (pConn->curStat == _PKG_HD_INIT)         // 连接建立起来肯定是这个状态，因为在ngx_get_connection()中已经把curStat成员赋值为_PKG_HD_INIT了
    {
        if (reco == m_iLenPkgHeader)        // 正好接收到完整包头，这里拆解包头
        {
            ngx_wait_request_handler_proc_p1(pConn, isflood);    // 调用专门针对包头处理完整的函数进行处理
        }
        else
        {
            // 收到的包头不完整--我们不可能预料每个包的长度，也不能预料各种拆包/粘包的情况，所以收不到完整的包头【也算是缺包】是有可能的
            pConn->curStat = _PKG_HD_RECVING;           // 接收包头中，包头不完整，继续接收包头
            pConn->precvbuf = pConn->precvbuf + reco;       // 注意接收后续包的内存往后走
            pConn->irecvlen = pConn->irecvlen - reco;       // 要接收的内容当然也要减少，以确保只收到完整的包头先

        }
        
    }
    else if (pConn->curStat == _PKG_HD_RECVING)         // 接收包头中，包头不完整，继续接收中，这个条件才会成立
    {
        if (pConn->irecvlen == reco) // 要求收到的宽度和我们实际收到的宽度相等
        {
            // 包头收完整了
            ngx_wait_request_handler_proc_p1(pConn, isflood);    // 调用专门针对包头处理的函数
        }
        else
        {
            // 包头还没有收完整，继续收包头
            // pConn->curStat = _PKG_HD_RECVING;        // 这里没有必要
            pConn->precvbuf = pConn->precvbuf + reco;       // 注意收后续包的内存往后走
            pConn->irecvlen = pConn->irecvlen - reco;       // 要收的内容自然要减少，以确保只收到完整的包头先

        }
        
    }
    else if (pConn->curStat == _PKG_BD_INIT)
    {
        // 包头刚好收完，准备收包体
        if (reco == pConn->irecvlen)
        {
            // 收到的宽度等于要接收的宽度，包体也收完整了
            if (m_floodAkEnable == 1)
            {
                // Flood攻击是否开启
                isflood = TestFlood(pConn);
            }
            
            ngx_wait_request_handler_proc_plast(pConn, isflood);
        }
        else
        {
            // 收到的宽度小于要收的宽度
            pConn->curStat = _PKG_BD_RECVING;
            pConn->precvbuf = pConn->precvbuf + reco;
            pConn->irecvlen = pConn->irecvlen - reco;
        }
        
    }
    else if (pConn->curStat == _PKG_BD_RECVING)
    {
        // 接收包体中，包体不完整，继续接收中
        if (pConn->irecvlen == reco)
        {
            // 包体接收完整了
            if (m_floodAkEnable == 1)
            {
                // FloodAkEnable是否开启
                isflood = TestFlood(pConn);
            }
            
            ngx_wait_request_handler_proc_plast(pConn, isflood);
        }
        else
        {
            // 包体没接收完整，继续接收
            pConn->precvbuf = pConn->precvbuf + reco;
			pConn->irecvlen = pConn->irecvlen - reco;
        }
        
    }

    if (isflood == true)
    {
        // 客户端flood服务器，则直接把客户端踢掉
        zdCloseSocketProc(pConn);
    }
    
    return;
}

// 接收数据专用函数，引入这个函数是为了方便，如果断线，错误子类的，这里直接 释放连接池中的连接，然后直接关闭socket，以免在其他函数中还要重复干这些是
// 参数c：连接池中的相关连接
// 参数buff：接收数据的缓冲区
// 参数buflen：要接收的数据大小
// 返回值：返回-1   则是有问题并且已经在这里把问题处理完毕了，本函数的调用者一般是可以直接return不做处理
        // 返回>0   则是表示实际收到的字节数
ssize_t CSocket::recvproc(lpngx_connection_t pConn, char *buff, ssize_t buflen)     // ssize_t是有符号整形，在32位机器上等同于int，在64位机器上等同于long int,  size_t就是无符号型的ssize_t
{
    ssize_t n;

    n= recv(pConn->fd, buff, buflen, 0);    // recv()系统函数。最后一个参数flag，一般为0
    if (n == 0)
    {
        // 客户端关闭【应该是正常完成了4次挥手】，这里就直接回收连接，关闭socket
        // ngx_log_stderr(0,"连接被客户端正常关闭[4路挥手关闭]！");
        // ngx_close_connection(pConn);
        // if (close(pConn->fd) == -1)
        // {
        //     ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocket::recvproc()中close(%d)失败!",pConn->fd);  
        // }
        // inRecyConnectQueue(pConn);
        zdCloseSocketProc(pConn);
        return -1;
    }

    // 客户端没断，走到这里
    if (n < 0)  // 这里被认为有错误发生
    {
        // EAGAIN和EWOULDBLOCK【EWOULDBLOCK这个错误码应该常用在惠普的系统上】应该是一样的值，表示没有收到数据。一般来讲，在ET模式下会出现这个错误，因为ET模式下是不停的recv，肯定有一个时刻会收到这个errno，但是LT模式下一般是来事件才会收，所以不会出现这个返回值
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // 这里认为LT模式不该出现这个errno，而且其实这个也不是一个错误，所以不当错误进行处理
            // 因为这个EAGAIN错误码是属于ET模式，不断的recv，直到把所有数据包收完了，收不到数据包了，recv()返回-1，errno才会变成EAGAIN
            // 但是LT模式下，是有数据的时候系统通知你来recv，所以你不会不停的一直recv，就不会出现这个错误
            // 注意这种错误不属于客户端的socket关闭，所以可以直接返回
            ngx_log_stderr(errno,"CSocket::recvproc()中errno == EAGAIN || errno == EWOULDBLOCK成立，出乎我意料！");//epoll为LT模式不应该出现这个返回值，所以直接打印出来瞧瞧
            return -1; //不当做错误处理，只是简单返回
        }

        // EINTR错误的产生：当阻塞于某个慢系统调用的一个进程捕获某个信号并且相应的信号处理函数返回时，该系统调用可能返回一个EINTR错误
        // 例如：在socket服务器端，设置了信号捕获机制，有子进程，当在父进程阻塞于慢系统调用时，由父进程吧捕获到了一个有效信号时，内核会致使accept返回一个EINTR错误（被中断的系统调用）
        if (errno == EINTR)     // 这个不算错误，参考官方nginx
        {
            // 我认为LT模式不该出现这个errno，而且这个其实也不是错误，所以不当做错误处理
            // 因为我们这里是非阻塞套接字，所以这里其实也不该出现这个错误
            ngx_log_stderr(errno,"CSocket::recvproc()中errno == EINTR成立，出乎我意料！");//epoll为LT模式不应该出现这个返回值，所以直接打印出来瞧瞧
            return -1; //不当做错误处理，只是简单返回
        }

        // 所有从这里走下来的错误，都认为是异常：意味着我们要关闭客户端套接字，要回收连接池中的连接
        
        if (errno == ECONNRESET)    //     #define ECONNRESET      108 /* Connection reset by peer (对等方重置连接) */
        {
            // 如果客户端没有正常关闭socket连接，却关闭了整个运行程序【也就是没有给服务器发送4次挥手包完成连接断开，而是直接发送rst包】那么就会产生整个错误
            // 10054（WSAECONNRESET）--远程程序正在连接的时候关闭会产生整个错误--远程主机强迫关闭了一个现有的连接
            // 算是常规错误【普通信息类型】日志都可以不用打印

            // 遇到的一些很普通的错误信息，都可以往这里加
        }
        else
        {
            // 能走到这里的，都表示错误，打印日志
            ngx_log_stderr(errno,"CSocket::recvproc()中发生错误，我打印出来看看是啥错误！");  //正式运营时可以考虑这些日志打印去掉
        }
        
        //ngx_log_stderr(0,"连接被客户端 非 正常关闭！");
        // ngx_close_connection(pConn);
        // if (close(pConn->fd) == -1)
        // {
        //     ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocket::recvproc()中close(%d)失败!",pConn->fd);  
        // }
        // inRecyConnectQueue(pConn);
        zdCloseSocketProc(pConn);
        return -1;
    }
    
    // 能走到这里的，就认为收到了有效数据
    return n;   // 返回收到的字节数

}

// 包头收完整后的处理，包处理阶段1
void CSocket::ngx_wait_request_handler_proc_p1(lpngx_connection_t pConn, bool &isflood)
{
    CMemory *p_memory = CMemory::GetInstance();

    LPCOMM_PKG_HEADER pPkgHeader;
    pPkgHeader = (LPCOMM_PKG_HEADER)pConn->dataHeadInfo;    // 正好收到包头时，包头信息肯定在dataHeadInfo里

    unsigned short e_pkgLen;
    e_pkgLen = ntohs(pPkgHeader->pkgLen);
    // 注意这里网络序转为本机序，所有传输到网络上的2字节数据，都要用htons()转为网络序，所有从网络上收到的2字节数据，都要用ntohs()转为本机序 
    // ntohs/htons的目的就是保证不同操作系统之间收发数据的正确性【不管是客户端、服务器是什么操作系统，发送的数字是多少，收到的就是多少】

    // 恶意包或者错误包的判断
    if (e_pkgLen < m_iLenPkgHeader)
    {
        // 伪装包、或者包错误，否者整个包长怎么可能比包头还小？（整个包长是包头+包体,就算包体为0字节，那么至少e_pkgLen == m_iLenPkgHeader）
        // 报文总长度 < 包头长度，认定非法用户，废包
        // 状态和接收位置都复原，这些字都有必要，因为有可能在其他状态比如_PKG_HD_RECVING状态调用这个函数
        pConn->curStat = _PKG_HD_INIT;
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = m_iLenPkgHeader;

    }
    else if (e_pkgLen > (_PKG_MAX_LENGTH-1000)) // 客户端发送来的包居然说包长度 > 29000 ? 判定为恶意包
    {
        // 恶意包，太大，认定为非法用户，废包【包头中说这个包总长度这么大，这不行】
        // 状态和接收位置都复原，这些值都有必要，因为有可能在其他状态比如_PKG_HD_RECVING状态调用这函数
        pConn->curStat = _PKG_HD_INIT;
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = m_iLenPkgHeader;
    }
    else
    {
        // 合法的包头，继续处理
        // 现在要分配内存，开始接收包体，因为包体总长度并不是固定的，所以内存肯定要new出来
        char *pTmpBuffer = (char *)p_memory->AllocMemory(m_iLenMsgHeader + e_pkgLen, false);    // 分配内存【长度是 消息头长度 + 包体长度】最后参数先给false，表示内存不需要memset
        pConn->ifnewrecvMem = true;         // 标记我们new了内存，将来在ngx_free_connextion()要进行内存回收
        pConn->pnewMemPointer = pTmpBuffer;     // 内存开始指针


        // 1）先填写消息头内容
        LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)pTmpBuffer;  // 消息头指向分配内存的首地址
        ptmpMsgHeader->pConn = pConn;                           // 记录连接池中的连接
        ptmpMsgHeader->iCurrsequence = pConn->iCurrsequence;    // 收到包时的连接池中连接序号记录到信息头中来，以备后续使用
        // 2）再填写包头内容
        pTmpBuffer += m_iLenMsgHeader;                      // 往后跳，跳过消息头，指向包头
                                                            // pTmpBuffer是char *类型，本身是1个字节， ，+=的话就会跳过m_iLenMsgHeader这么多个字节
        memcpy(pTmpBuffer, pPkgHeader, m_iLenPkgHeader);    // 直接把收到的包头拷贝进来

        // 注意一个点，代码走到这里，只是接受完了包头，还没有开始接收包体
        if (e_pkgLen == m_iLenPkgHeader)
        {
            // 该报文只有包头无包体【允许一个包只有包头，没有包体】
            // 这相当于收完整了，则直接接入消息队列带后续业务逻辑线程去处理
            if (m_floodAkEnable == 1)
            {
                // flood攻击检测是否开启
                isflood = TestFlood(pConn);
            }
            
            ngx_wait_request_handler_proc_plast(pConn, isflood);
        }
        else
        {
            // 开始接收包体
            pConn->curStat = _PKG_BD_INIT;                      // 单前状态发生改变，包头刚好收完，准备接收包体
            pConn->precvbuf = pTmpBuffer + m_iLenPkgHeader;     // pTmpBuffer指向包头，这里 + m_iLenPkgHeader后指向包体位置
            pConn->irecvlen = e_pkgLen - m_iLenPkgHeader;       // e_pkgLen是整个包【包体+包头】大小 - m_iLenPkgHeader【包头】 = 包体
        }

    }
    
    return;
    
}

// 收到一个完整的包后的处理函数 【plast表示最后阶段】
void CSocket::ngx_wait_request_handler_proc_plast(lpngx_connection_t pConn, bool &isflood)
{
    // 把这段内存放到消息队列中来
    // int irmqc = 0;    // 消息队列中当前消息数量
    // inMsgRecvQueue(pConn->pnewMemPointer, irmqc);
    // 注意这里，我们把这段new出来的内存放到消息队列中，那么后续这段内存就不归连接池管理了
    // 也就是说，这段内存的释放就不能再连接池中进行释放了，而应该放到具体的业务函数中进行处理
    // 所以下面才会把ifnewrecvMem内存释放标记设置为false，指向内存的指针也设置为空NULL
    // ---------这里可能考虑触发业务逻辑，怎么触发业务逻辑，后续实现

    // 激发线程池中的某个线程来处理业务逻辑
    // g_threadPool.Call(irmqc);

    if (isflood ==  false)
    {
        g_threadpool.inMsgRecvQueueAndSignal(pConn->precvMemPointer);       // 入消息队列并触发线程处理消息
    }
    else
    {
        // 对于有攻击倾向的恶意用户，先把他的包丢掉
        CMemory *p_memory = CMemory::GetInstance();
        p_memory->FreeMemory(pConn->precvMemPointer);       // 直接释放清理内存，根本不会加入到消息队列
    }
    

    // g_threadpool.inMsgRecvQueueAndSignal(pConn->pnewMemPointer);    // 入消息队列并触发线程处理消息

    pConn->ifnewrecvMem     = false;            // 内存不再需要释放，因为你收完整了包，这个包被上面调用inMsgRecvQueue()移入消息队列，那么释放内存就属于业务逻辑去干， 不需要回收连接到连接池中做了
    pConn->pnewMemPointer   = NULL;
    pConn->curStat          = _PKG_HD_INIT;     // 收包状态机的状态恢复为原始态，为收下一个包做准备
    pConn->precvbuf         = pConn->dataHeadInfo;  // 设置好收包的位置
    pConn->irecvlen         = m_iLenPkgHeader;  // 设置好要接收的数据的大小
    return;
}

// 当收到一个完整包之后，将完整包移入消息队列，这个包在服务器端应该是 消息头+包头+包体 格式
// void CSocket::inMsgRecvQueue(char *buf, int &irmqc)     // buf这段内存 ： 消息头 + 包头 + 包体
// {

//     CLock lock(&m_recvMessageQueueMutex);       // 自动加锁很方便，不需要手动去解锁
//     m_MsgRecvQueue.push_back(buf);              // 入消息队列
//     ++m_iRecvMsgQueueCount;                     // 收消息队列数字+1，
//     irmqc = m_iRecvMsgQueueCount;               // 接收消息队列当前信息数量保存到irmqc中

//     // m_MsgRecvQueue.push_back(buf);

//     // ....其他功能待扩充，记住一点;这里的内存是需要进行释放的，-----释放代码后续增加
//     // ....而且处理逻辑应该引入多线程，所以这里要考虑临界问题
    
//     // 临时在这里调用一下该函数，以防止接收消息队列过大
//     // tmpoutMsgRecvQueue();   // ... 临时，后续会取消这行代码

//     // 为了测试方便，因为本函数一位着收到了这个完整的数据包，所以这里打印一个信息
//     // ngx_log_stderr(0,"非常好，收到了一个完整的数据包【包头+包体】！");

// }

// //从消息队列中把一个包提取出来以备后续处理
// char *CSocket::outMsgRecvQueue() 
// {
//     CLock lock(&m_recvMessageQueueMutex);	//互斥
//     if(m_MsgRecvQueue.empty())
//     {
//         return NULL; //也许会存在这种情形： 消息本该有，但被干掉了，这里可能为NULL的？        
//     }
//     char *sTmpMsgBuf = m_MsgRecvQueue.front(); //返回第一个元素但不检查元素存在与否
//     m_MsgRecvQueue.pop_front();                //移除第一个元素但不返回	
//     // 注意这里pop_front()，这个pop_front只是把这个指针从list容器中移出来，可不是把这个元素所对应的内存给进行释放，对于这里块的理解千万不能糊涂
//     // 因为我们在收数据包的时候，会new一个内存，然后把这个内存的指针放到这个list中，这个指针并没有因为pop_front而把实际的内存释放掉
//     // 所以下面直接把这块内存作为返回值return出去
//     --m_iRecvMsgQueueCount;                    //收消息队列数字-1
//     return sTmpMsgBuf;                         
// }


// 临时函数。用于将Msg中的消息进行释放
// void CSocket::tmpoutMsgRecvQueue()
// {
//     // 日后可能会引入outMsgRecvQueue()，这个函数可能需要临界
//     if (m_MsgRecvQueue.empty())     // 没有消息直接退出
//     {
//         return;
//     }
//     int size = m_MsgRecvQueue.size();
//     if (size < 1000)    // 消息不超过1000条就先不处理
//     {
//         return;
//     }

//     // 消息达到1000条
//     CMemory *p_memory = CMemory::GetInstance();
//     int cha = size - 500;
//     for (int i = 0; i < cha; ++i)
//     {
//         // 一次干掉一堆
//         char *sTmpMsgBuf = m_MsgRecvQueue.front();//返回第一个元素但不检查元素存在与否
//         m_MsgRecvQueue.pop_front();               //移除第一个元素但不返回	
//         p_memory->FreeMemory(sTmpMsgBuf);         //先释放掉把；
//     }
    
//     return;
    
// }


//消息处理线程主函数，专门处理各种接收到的TCP消息
//pMsgBuf：发送过来的消息缓冲区，消息本身是自解释的，通过包头可以计算整个包长
//         消息本身格式【消息头+包头+包体】 
void CSocket::threadRecvProcFunc(char *pMsgBuf)
{

    return;
}

// 设置数据发送是的写处理函数，当数据可写时，epoll通知我们，在int CSocket::ngx_epoll_process_events(int timer)中调用此函数
// 能够走到这里，数据就是没发送完毕，要继续发送
void CSocket::ngx_write_request_handler(lpngx_connection_t pConn)
{
    CMemory *p_memory = CMemory::GetInstance();

    // 这些代码可以参考 void *CSocket::ServerSendQueueThread(void * threadData)
    ssize_t sendsize = sendproc(pConn, pConn->precvbuf, pConn->isendlen);

    if (sendsize > 0 && sendsize != pConn->isendlen)
    {
        // 没有完全发送完毕，数据只发出去了一部分，那么发送到了哪里？剩余多少？继续记录，方便下次sendproc()时使用
        pConn->psendbuf = pConn->psendbuf + sendsize;
        pConn->isendlen = pConn->isendlen - sendsize;
        return;
    }
    else if (sendsize == -1)
    {
        // 这不太可能，可以发送数据时系统通知我发数据，我发送数据时，却告知我发送缓冲区满？
        ngx_log_stderr(errno,"CSocket::ngx_write_request_handler()时if(sendsize == -1)成立，这很怪异。"); //打印个日志，别的先不干啥
        return;
    }
    
    if (sendsize > 0 && sendsize == pConn->isendlen)    // 成功发送完毕，做个通知
    {
        // 如果是成功的发送完毕数据，则把写事件通知从epoll中移除，其他情况，那就是断线了，等着系统把连接从红黑树中移除即可
        if (ngx_epoll_oper_event(
            pConn->fd,              // socket句柄
            EPOLL_CTL_MOD,          // 事件类型，这里是修改【因为我们准备减去写通知】
            EPOLLOUT,               // 标志，这里代表要减去的标志，EPOLLOUT：可写【可写的时候通知我】
            1,                      // 对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数， 0：增加，1：减去， 2：完全覆盖
            pConn                   // 连接池中的连接
        ) == -1)
        {
            // 有这种情况发生？先do nothing
             ngx_log_stderr(errno,"CSocket::ngx_write_request_handler()中ngx_epoll_oper_event()失败。");
        }
        ngx_log_stderr(0,"CSocket::ngx_write_request_handler()中数据发送完毕，很好。"); //做个提示吧，线上可以干掉
    }
    // 能走下来的，要么是数据发送完毕了，要么是对端断开了，开始执行收尾工作

    // 数据发送完毕，或者把需要发送的数据干掉，都说明发送缓冲区可能有地方了，让发送线程往下走，判断能否发送新数据
    if (sem_post(&m_semEventSendQueue) == -1)
    {
        ngx_log_stderr(0,"CSocket::ngx_write_request_handler()中sem_post(&m_semEventSendQueue)失败.");
    }
    
    p_memory->FreeMemory(pConn->psendMemPointer);       // 释放内存
    pConn->psendMemPointer = NULL;
    --pConn->iThrowsendCount;                           // 建议放在最后执行
    return;
}

// 发送数据专用函数，返回本次发送的字节数
// 返回 > 0 成功发送了一些字节
// = 0， 估计对方断线了
// = -1，errno == EAGAIN,本方发送缓冲区满了
// = -2，errno != EAGAIN != EWOULDBLOCK != EINTR 一般认为都是对端断开连接的错误
ssize_t CSocket::sendproc(lpngx_connection_t c, char *buff, ssize_t size) // ssize_t是有符号整型，在32位机器上等同与int，在64位机器上等同与long int，size_t就是无符号型的ssize_t
{
    // 这里借鉴了官方nginx函数ngx_unix_send()的写法
    ssize_t n;

    for (;;)
    {
        n = send(c->fd, buff, size, 0);     // send()系统函数，最后一个参数flag，一般为0
        if (n > 0)  // 成功发送了一些数据
        {
            // 发送成功一些数据，但是发送了多少，我这里并不关心，也不需要再次send
            // 这里有两种情况
            // （1）n == size 也就是想发送多少都发送成功了，也不需要再次send()
            // （2）n < size 没发送完毕，那肯定是发送缓冲区满了，所以也不必重新send发送，直接返回
            return n;       // 返回本次发送的字节数
        }

        if (n == 0)
        {
            // send()返回0？一般recv()返回0表示断开，send()返回0，这里就直接返回0【让调用者处理】，这里我认为send()返回0，要么是你发送的字节是0，要么是对端可能断开
            // 网上查资料发现：send == 0表示超时，对方主动关闭了连接过程
            // 我们写代码要遵循一个原则，连接断开，我们并不在send动作里处理诸如关闭socket这种动作，集中到recv那里进行处理，否者send,recv都处理连接断开，关闭socket会乱套
            // 连接断开epoll会通知并且 recvproc()里面会处理，不在这里处理
            return 0;
        }
        
        if (errno == EAGAIN)    // 这个东西应该等于 EWOULDBLOCK
        {
            // 内核缓冲区满，这个不算错误
            return -1;      // 表示发送缓冲区满了
        }

        if (errno == EINTR)
        {
            // 这个应该也不算错误，收到某个信号导致send产生这个错误？
            // 参考官方nginx的写法，打印一个日志，其他什么也不干，那就是等到下一次for循环重新send试一次
            ngx_log_stderr(errno,"CSocket::sendproc()中send()失败.");  //打印个日志看看啥时候出这个错误
            //其他不需要做什么，等下次for循环吧            
        }
        else
        {
            // 走到这里表示是其他错误码，都表示错误，错误这里也不断开socket，依旧等待recv()来统一处理，因为是多线程，send()也处理断开，recv()也处理断开，很难处理好
            return -2;
        }
        
    }
    
}
