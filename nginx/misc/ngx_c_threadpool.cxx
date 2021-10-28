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
        // 注意这里 ThreadItem是线程池中的一个结构，代表一个线程，所以我们每创建一个线程就需要new这样一个对象
        // 这里这个this指针是整个线程池类的对象指针（代表的就是整个线程池这个大对象），这里注意去看ThreadItem的构造函数，它在构造函数里对this进行了列表初始化
        // 将我们这里创建线程对象的线程池的this指针传递给了每个线程ThreadItem的成员变量 _pThis
        // 这样写有什么好处呢，因为这里ThreadItem代表一个线程，我们把线程池类的指针通过这个参数pthis传递给线程类ThreadItem的成员变量，用这个成员变量来保存线程池对象的指针，
        // 那么我日后对于每一个创建成功的线程，就可以通过线程对象的这个_pThis访问到这个线程池管理里对象（访问到线程池）
        m_threadVector.push_back(pNew = new ThreadItem(this));      // 创建一个新线程对象，并放入容器中
        err = pthread_create(&pNew->_Handle, NULL, ThreadFunc, pNew);   // 创建线程，错误不能返回到errno，一般返回错误码
        // 注意：pthread_create创建线程，需要传入一个pthread_t类型的变量来保存创建出来的线程句柄
        // ThreadFunc:线程入口函数
        // pNew:线程入口函数的参数
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
    // 注意这里：void * threadData 这个是我们在创建线程的时候绑定的内存，然后在下面的代码中，要把它转换还原 pThread
    // 这里为什么会说有个转换还原的说法，因为传进来的threadData指针，他里面的这个_pThis指向的是线程池的对象指针

    // 然后这个线程条目 ThreadItem 他里面是有个成员 ifrunning 
    // bool            ifrunning;          // 标记是否正式启动起来，启动起来后，才允许调用StopAll()来释放
    // 这个ifrunning缺省值为false，表示你这个线程没有在运行，一旦你这个线程执行起来之后，只有当他 执行到下面这行 pthread_cond_wait 的时候，我们才认为你整个线程启动起来了
    // 那么对于线程池里面所有的线程启动，都要执行到 pthread_cond_wait 这行，ifrunning才为true,那我才认为你这个线程池完全启动起来了

    // 这个静态成员函数，是不存在this指针的
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CThreadPool *pThreadPoolObj = pThread->_pThis;
    // pThreadPoolObj这个就是该线程池类对象的指针

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
                pThread->ifrunning = true;  // 标记为true了才允许调用StopAll(): 测试中如果发现Create()和StopAll()紧挨着调用，就会导致线程混乱，所以每个线程必须执行到这里，才认为是启动了
            }

            // ngx_log_stderr(0,"执行了pthread_cond_wait-------------begin");
            // 刚开始执行pthread_cond_wait()的时候，会卡在这里，而且m_pthreadMutex会被释放掉
            pthread_cond_wait(&m_pthreadCond, &m_pthreadMutex);     // 整个服务器程序刚初始化的时候，所有线程必然是卡在这里等待的
            // ngx_log_stderr(0,"执行了pthread_cond_wait-------------end");
            
        }
        
        // 能走下来的，必然是 拿到了真正的消息队列中的数据， 或者 m_shutdown == true

        // jobbuf = g_socket.outMsgRecvQueue();        // 从消息队列中取出消息、
        // if (jobbuf == NULL && m_shutdown == false)
        // {
        //     // 消息队列为空，并且不要求退出
        //     // pthread_cond_wait()阻塞调用线程直到指定的条件有信号（signaled)
        //     // 该函数应该在互斥量锁定的时候调用，当在等待时会自动巨额所互斥量【这是重点】在信号被发送，线程被激活后，互斥量会自动被锁定，当线程结束时，由程序员负责解锁互斥量
        //     // 说白了，某个地方调用了pthread_cond_signal(&m_pthreadCond); 这个pthread_cond_wait就会走下去

        //     ngx_log_stderr(0,"--------------即将调用pthread_cond_wait,tid=%d--------------",tid);

        //     if (pThread->ifrunning == false)
        //     {
        //         pThread->ifrunning = true;      // 标记为true了才允许调用StopAll(),: 测试中发现如果Create()和StopAll()紧挨着使用，就会导致线程混乱，所以每个线程必须执行到这里，才认为是启动成功了；
        //     }

        //     err = pthread_cond_wait(&m_pthreadCond, &m_pthreadMutex);
        //     if(err != 0) ngx_log_stderr(err,"CThreadPool::ThreadFunc()pthread_cond_wait()失败，返回的错误码为%d!",err);//有问题，要及时报告


        //     ngx_log_stderr(0,"--------------调用pthread_cond_wait完毕,tid=%d--------------",tid);
            
        // }

        // if(!m_shutdown)  // 如果这个条件成立，表示肯定是拿到了消息队列中的数据，要去干活了，就说明正在运行的线程数量要+1
        // ++m_iRunningThreadNum;   // 因为这里是互斥的，所以这个+是OK的

        err = pthread_mutex_unlock(&m_pthreadMutex);    // 先解锁mutex
        if (err!=0)
        {
            ngx_log_stderr(err,"CThreadPool::ThreadFunc()pthread_cond_wait()失败，返回的错误码为%d!",err);//有问题，要及时报告
        }

        // 先判断线程退出这个条件
        if (m_shutdown)
        {
            if (jobbuf != NULL)
            {
                // 这个条件在这里应该不成立，不过加上也无所谓【后来又感觉也可能会成立尤其是当要退出的时候】
                p_memory->FreeMemory(jobbuf);
            }
            break;
        }
        
        // 能走到这里的，就是表示有数据可以处理的
        ++pThreadPoolObj->m_iRunningThreadNum;  // 原子+1， 这比互斥量要快很多

        // g_socket.threadRecvProcFunc(jobbuf); // 处理消息队列中来的消息

        ngx_log_stderr(0,"执行开始---begin,tid=%ui!",tid);
        sleep(5); //临时测试代码
        ngx_log_stderr(0,"执行结束---end,tid=%ui!",tid);

        p_memory->FreeMemory(jobbuf);           // 释放消息内存
        --pThreadPoolObj->m_iRunningThreadNum;  // 原子-1
        
    }

    // 能走出来表示整个程序要结束，怎么判断所有线程都结束？
    return (void*)0;
    
}

// 停止所有线程【等待结束线程中所有线程，该函数返回后，应该是所有线程池中的线程都结束了】
void CThreadPool::StopAll()
{
    // （1）已经调用过了，就不要重复调用了
    if (m_shutdown == true)
    {
        return;
    }

    m_shutdown = true;
    

    //（2）唤醒等待该条件 【卡在pthread_cond_wait()】的所有线程，一定要在条件改变状态后再给线程发信号
    int err = pthread_cond_broadcast(&m_pthreadCond);
    if (err != 0)
    {
        // 这里肯定有问题，打印日志
        ngx_log_stderr(err,"CThreadPool::StopAll()中pthread_cond_broadcast()失败，返回的错误码为%d!",err);
        return;
    }

    // （3）等待线程返回
    std::vector<ThreadItem*>::iterator iter;
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); ++iter)
    {
        pthread_join((*iter)->_Handle, NULL);   // 等待一个线程终止
    }

    // 流程走到这里，那么所有的线程池中的线程肯定都返回了
    pthread_mutex_destroy(&m_pthreadMutex);
    pthread_cond_destroy(&m_pthreadCond);

    // (4) 释放new出来的threadItem【线程池中的线程】
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        if (*iter)
        {
            delete *iter;
        }
        
    }
    
    m_threadVector.clear();

    ngx_log_stderr(0,"CThreadPool::StopAll()成功返回，线程池中线程全部正常结束!");
    return; 
    
    
}

// 来任务了，调用一个线程池中的线程来干活
void CThreadPool::Call(int irmqc)
{
    //ngx_log_stderr(0,"m_pthreadCondbegin--------------=%ui!",m_pthreadCond);  //数字5，此数字不靠谱
    //for(int i = 0; i <= 100; i++)
    //{
    int err = pthread_cond_signal(&m_pthreadCond);      // 唤醒一个等待该条件的线程，也就是可以唤醒卡在pthread_cond_wait()的线程
    if (err!=0)
    {
        // 说明这里有问题
        ngx_log_stderr(err,"CThreadPool::Call()中pthread_cond_signal()失败，返回的错误码为%d!",err);
    }
    //}
    //唤醒完100次，试试打印下m_pthreadCond值;
    //ngx_log_stderr(0,"m_pthreadCondend--------------=%ui!",m_pthreadCond);  //数字1

    // （1）如果当前的工作线程全部都忙，则要告警
    // bool ifallthreadbusy = false;
    if (m_iThreadNum == m_iRunningThreadNum)    // 线程池中线程总量，跟当前正在干活的线程数量一样，说明所有线程都忙碌起来，线程不够用了
    {
        // 线程不够用了
        // ifallthreadbusy = true;
        time_t currtime = time(NULL);
        if (currtime - m_iLastEmgTime > 10) // 最少间隔10秒才报一次线程池中线程不够用的问题
        {
            // 两次报告之间的间隔必须超过10秒，不然如果一直出现当前工作线程全忙，频繁报告日志
            m_iLastEmgTime = currtime;  // 更新时间
            // 写日志，通知这种紧急情况给用户，用户要考虑增加线程池中的线程数量
            ngx_log_stderr(0,"CThreadPool::Call()中发现线程池中当前空闲线程数量为0，要考虑扩容线程池了!");
        }
        
    }

    /*
    //-------------------------------------------------------如下内容都是一些测试代码；
    //唤醒丢失？--------------------------------------------------------------------------
    //(2)整个工程中，只在一个线程（主线程）中调用了Call，所以不存在多个线程调用Call的情形。
    if(ifallthreadbusy == false)
    {
        //有空闲线程  ，有没有可能我这里调用   pthread_cond_signal()，但因为某个时刻线程曾经全忙过，导致本次调用 pthread_cond_signal()并没有激发某个线程的pthread_cond_wait()执行呢？
           //我认为这种可能性不排除，这叫 唤醒丢失。如果真出现这种问题，我们如何弥补？
        if(irmqc > 5) //我随便来个数字比如给个5吧
        {
            //如果有空闲线程，并且 接收消息队列中超过5条信息没有被处理，则我总感觉可能真的是 唤醒丢失
            //唤醒如果真丢失，我是否考虑这里多唤醒一次？以尝试逐渐补偿回丢失的唤醒？此法是否可行，我尚不可知，我打印一条日志【其实后来仔细相同：唤醒如果真丢失，也无所谓，因为ThreadFunc()会一直处理直到整个消息队列为空】
            ngx_log_stderr(0,"CThreadPool::Call()中感觉有唤醒丢失发生，irmqc = %d!",irmqc);

            int err = pthread_cond_signal(&m_pthreadCond); //唤醒一个等待该条件的线程，也就是可以唤醒卡在pthread_cond_wait()的线程
            if(err != 0 )
            {
                //这是有问题啊，要打印日志啊
                ngx_log_stderr(err,"CThreadPool::Call()中pthread_cond_signal 2()失败，返回的错误码为%d!",err);
            }
        }
    }  //end if

    //(3)准备打印一些参考信息【10秒打印一次】,当然是有触发本函数的情况下才行
    m_iCurrTime = time(NULL);
    if(m_iCurrTime - m_iPrintInfoTime > 10)
    {
        m_iPrintInfoTime = m_iCurrTime;
        int irunn = m_iRunningThreadNum;
        ngx_log_stderr(0,"信息：当前消息队列中的消息数为%d,整个线程池中线程数量为%d,正在运行的线程数量为 = %d!",irmqc,m_iThreadNum,irunn); //正常消息，三个数字为 1，X，0
    }
    */
    return;
    
}

//唤醒丢失问题，sem_t sem_write;
//参考信号量解决方案：https://blog.csdn.net/yusiguyuan/article/details/20215591  linux多线程编程--信号量和条件变量 唤醒丢失事件
