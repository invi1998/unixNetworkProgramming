#ifndef __NGX_THREADPOOL_H__
#define __NGX_THREADPOOL_H__

#include <vector>
#include <pthread.h>
#include <atomic>   //c++11里的原子操作

// 线程池相关类
class CThreadPool
{
public:
    CThreadPool();
    ~CThreadPool();

public:
    bool Create(int threadNum);                 // 创建该线程池中所有的线程
    void StopAll();                             // 使线程池中所有的线程退出
    void Call(int irmqc);                       // 来任务了，调一个线程池中的线程下来干活
    void inMsgRecvQueueAndSignal(char *buf);    // 收到一个完整消息后，入消息队列，并触发线程池中的线程来处理该消息

private:
    static void * ThreadFunc(void *threadData); // 新线程的线程回调函数
    void clearMsgRecvQueue();                   // 清理接受消息队列
    // char *outMsgRecvQueue();                    // 将一个消息出消息队列，不需要直接在ThreadFunc()中处理

private:
    // 定义一个线程池中的 线程 的结构，以后可以做一些统计之类的功能扩展，所以引入这么一结构 来代表线程，更方便扩展
    struct ThreadItem
    {
        pthread_t       _Handle;            // 线程句柄
        CThreadPool     *_pThis;            // 记录线程池的指针
        bool            ifrunning;          // 标记是否正式启动起来，启动起来后，才允许调用StopAll()来释放

        // 构造函数
        ThreadItem(CThreadPool *pthis):_pThis(pthis),ifrunning(false){}
        // 注意这里：pthis是一个线程池指针，然后在初始化列表里将这个线程池指针赋予了_pThis,
        // 这样写有什么好处呢，因为这里ThreadItem代表一个线程，我们把线程池类的指针通过这个参数pthis传递给线程类ThreadItem的成员变量，用这个成员变量来保存线程池对象的指针，
        // 那么我日后对于每一个创建成功的线程，就可以通过线程对象的这个_pThis访问到这个线程池管理里对象（访问到线程池）

        // 析构函数
        ~ThreadItem(){}
    };

private:
    static pthread_mutex_t      m_pthreadMutex;         // 线程同步互斥量/也叫做线程同步锁
    static pthread_cond_t       m_pthreadCond;          // 线程同步条件变量
    static bool                 m_shutdown;             // 线程退出标志，false不退出，true退出

    int                         m_iThreadNum;           // 要创建的线程数量

    // int                         m_iRunningThreadNum;    // 线程数，运行中的线程数
    std::atomic<int>            m_iRunningThreadNum;    // 线程数，运行中的线程数，原子操作
    time_t                      m_iLastEmgTime;         // 上次发生线程不够用【紧急事件】的时间，防止日志爆的太频繁
    //time_t                     m_iPrintInfoTime;      //打印信息的一个间隔时间，我准备10秒打印出一些信息供参考和调试
    //time_t                     m_iCurrTime;           //当前时间

    std::vector<ThreadItem *>   m_threadVector;         // 线程 容器，容器里就是各个线程

    // 接受消息队列相关
    std::list<char *>           m_MsgRecvQueue;         // 接收数据消息队列
    int                         m_iRecvMsgQueueCount;   // 接收消息队列大小

};

#endif

