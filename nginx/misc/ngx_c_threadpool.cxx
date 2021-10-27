// 和线程池有关的函数

#include <stdarg.h>
#include <unistd.h> // usleep

#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_threadpool.h"
#include "ngx_c_memory.h"
#include "ngx_macro.h"

// 静态成员初始化
pthread_mutex_t CThreadPool::m_pthreadMutex = PTHREAD_MUTEX_INITIALIZER;    // #define PTHREAD_MUTEX_INITIALIZER ((pthread_mutex_t) -1)
pthread_cond_t  CThreadPool::m_pthreadCond  = PTHREAD_COND_INITIALIZER;     // #define PTHREAD_COND_INITIALIZER ((pthread_cond_t) -1)
bool CThreadPool::m_shutdown = false;                                       // 刚开始标记整个线程池的线程是不退出的

// 构造函数
CThreadPool::CThreadPool()
{
    m_iRunningThreadNum = 0;            // 正在运行的线程，开始给一个0.【注意这种写法，原子的对象给0也可以直接赋值，当整形变量来使用】
    m_iLastEmgTime = 0;                 // 上次报告线程不够用了的时间
    // m_iPrintInfoTime = 0;               // 上次打印参考消息的时间

}

// 析构函数
CThreadPool::~CThreadPool()
{
    // 资源释放统计放在StopAll()中进行
}

// 创建线程池中的线程， 要手动调用，不要在构造函数里调用
// 返回值：所有的线程都创建成功则返回true，出现错误则返回false
bool CThreadPool::Create(int threadNum)
{
    ThreadItem *pNew;
    int err;

    m_iThreadNum = threadNum;   // 保存要创建的线程数量

    for (int i = 0; i < m_iThreadNum; ++i)
    {
        m_threadVector.push_back(pNew = new ThreadItem(this));      // 创建一个新线程对象，并放入容器中
        err = pthread_create(&pNew->_Handle, NULL, ThreadFunc, pNew);   // 创建线程，错误不能返回到errno，一般返回错误码
        if (err != 0)
        {
            // 创建线程有错
            ngx_log_stderr(err,"CThreadPool::Create()创建线程%d失败，返回的错误码为%d!",i,err);
            return false;
        }
        else
        {
            //创建线程成功
            //ngx_log_stderr(0,"CThreadPool::Create()创建线程%d成功,线程id=%d",pNew->_Handle);
        }
        
    }

    // 必须保证每个线程都启动并运行到pthread_cond_wait()，本函数才返回，只有这样，这几个线程才能进行后续的正常工作
    std::vector<ThreadItem*>::iterator iter;

lblfor:
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); ++iter)
    {
        if ((*iter)->ifrunning == false)    // 这个条件保证所有线程完全启动起来，以保证整个线程池中线程正常工作
        {
            // 这说明存在没有完全启动的线程
            usleep(100*1000);               // 单位是 微秒，又因为1毫秒=1000微妙，所以 100 *1000 = 100毫秒
            goto lblfor;
        }
        
    }

    return true;
    
}


// 线程入口函数，当用pthread_create()创建线程后，这个ThreadFunc()函数都会被立即执行
void * CThreadPool::ThreadFunc(void * threadData)
{
    // 这个静态成员函数，是不存在this指针的
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CThreadPool *pThreadPoolObj = pThread->_pThis;

    char *jobbuf = NULL;
    CMemory *p_memory = CMemory::GetInstance();
    int err;

    pThread_t tid = pThread_self();     // 获取线程自身ID，以方便调试打印
    while (true)
    {
        // 线程用pthread_mutex_lock()函数去锁定指定的mutex变量，若该mutex已经被另一个线程锁定了，该调用将会阻塞线程直到mutex被解锁
        err = pThread_mutex_lock(&m_pthreadMutex);
        if (err!=0)
        {
            // 有问题，及时打印
            ngx_log_stderr(err,"CThreadPool::ThreadFunc()pthread_mutex_lock()失败，返回的错误码为%d!",err);
        }

        // 一下这行程序写法十分重要，必须要用while这种写法
        // 因为：pthread_cond_wait()是个值得注意的函数，调用一次pthread_cond_signal()可能会唤醒多个【惊群】【官方描述是: 至少一个/pthread_cond_signal在处理器上可能同时唤醒多个线程】
        // 虚假唤醒
        // 条件变量/wait()/notify_one()/notify_all()，其实跟这里的pthread_cond_wait()/pthread_cond_signal()/pthread_cond_broadcast()非常类似
        // pthread_cond_wait()函数，如果只有一条消息，唤醒了两个线程干活，那么其中有一个线程拿不到消息，那如果不用while写，就会出问题，所以被惊醒后必须再次调用while拿消息，拿到消息才能走下来
        while ((jobbuf = g_socket.outMsgRecvQueue()) == NULL && m_shutdown == false)
        {
            // 如果这个pthread_cond_wait被唤醒【被唤醒后程序执行流程往下走的前提是拿到了锁--官方：pthread_cond_wait()返回时，互斥量再次被锁住】
            // 那么会立即再次执行g_socket.outMsgRecvQueue()。如果拿到了一个NULL，则继续在这里wait()着
            if (pthread->ifrunning == false)
            {
                pThread->ifrunning = 
            }
            
        }
        
        
    }
    
}
