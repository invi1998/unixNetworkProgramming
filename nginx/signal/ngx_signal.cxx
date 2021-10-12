//和信号有关的函数放这里
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>    //信号相关头文件 
#include <errno.h>     //errno

#include "ngx_macro.h"
#include "ngx_func.h" 

// 定义一个和信号处理有关的结构体   ngx_signal_t
typedef struct
{
    int         signo;          // 信号对应的数字编号，每个信号都有对应的#define
    const char  *signame;       // 信号对应的中文名字，比如SIGHUP

    // 信号处理函数，这个函数由我们自己提供，但是他的参数和返回值都是固定的【操作系统就这样要求】
    void (*handler)(int signo, siginfo_t *siginfo, void *ucontext);     // 函数指针，siginfo_t：系统定义的结构
}ngx_signal_t;


// 声明一个信号处理函数(函数指针)
static void ngx_signal_handler(int signo, siginfo_t *siginfo, void *ucontext);  // static表示该函数只在当前文件内可见


// 数组。定义本系统中处理的各种信号，我们取一小部分nginx中的信号，并没有全部搬移到这里，日后若有需要可以根据情况增加
// 在实际业务中，需要把能想到的要处理的信号都弄进来
ngx_signal_t  signals[] = {
    // signo      signame             handler
    { SIGHUP,    "SIGHUP",           ngx_signal_handler },        //终端断开信号，对于守护进程常用于reload重载配置文件通知--标识1
    { SIGINT,    "SIGINT",           ngx_signal_handler },        //标识2   
	{ SIGTERM,   "SIGTERM",          ngx_signal_handler },        //标识15
    { SIGCHLD,   "SIGCHLD",          ngx_signal_handler },        //子进程退出时，父进程会收到这个信号--标识17
    { SIGQUIT,   "SIGQUIT",          ngx_signal_handler },        //标识3
    { SIGIO,     "SIGIO",            ngx_signal_handler },        //指示一个异步I/O事件【通用异步I/O信号】
    { SIGSYS,    "SIGSYS, SIG_IGN",  NULL               },        //我们想忽略这个信号，SIGSYS表示收到了一个无效系统调用，如果我们不忽略，进程会被操作系统杀死，--标识31
                                                                  //所以我们把handler设置为NULL，代表 我要求忽略这个信号，请求操作系统不要执行缺省的该信号处理动作（杀掉我）
    //...日后根据需要再继续增加
    { 0,         NULL,               NULL               }         //信号对应的数字至少是1，所以可以用0作为一个特殊标记
};


// 初始化信号的函数，用于注册信号处理程序
// 、返回值： 0 成功 -1 失败
int ngx_init_signals()
{
    ngx_signal_t        *sig;       // 指向自定义的结构数组的指针
    struct sigaction    sa;         // sigaction: 系统定义的跟信号有关的一个结构，后续调用系统的sigaction()函数都用这个同名的结构

    for (sig = signals; sig->signo != 0; sig++)
    {
        // 注意这里：现在要把一堆信息往 变量 sa 对应的结构里弄
        memset(&sa, 0, sizeof(struct sigaction));

        if(sig->handler)    // 如果信号处理函数不为空，那么这就表示我需要定义自己的信号处理函数
        {
            sa.sa_sigaction = sig->handler;
            // sa->sigaction: 指定信号处理函数，注意sa_sigaction也是函数指针，这个是系统定义的结构sigaction中的一个成员（函数指针成员）
            sa.sa_flags = SA_SIGINFO;
            // sa_flags: int型 指定信号的一些选项，设置了该标记（SA_SIGINFO) 就表示信号附带的参数可以被传送到信号处理函数中
            // 说白了就是你想让 sa.sa_sigaction 指定的信号处理程序生效，那就把sa_flags 设置为 SA_SIGINFO

        }
        else
        {
            sa.sa_handler = SIG_IGN;
            // sa_handler:这个标记 SIG_IGN 给到sa_handler成员，表示忽略信号的处理程序，否者操作系统的缺省信号处理程序可能会把这个进程杀掉
            // 其实sa_handler和sa_sigaction都是一个函数指针用来表示信号处理程序，只不过这两个函数指针他们的参数不一样，sa_sigaction带的参数多，信息量大
            // 而sa_handler带的参数少，信息量少。如果你想用sa_sigaction，那么你就需要把sa_flags设置为SA_SIGINFO
        }

        sigemptyset(&sa.sa_mask);
        // 比如这里。处理某个信号SIGUSR1信号时不希望接收到SIGUSR2信号， 那么就可以用诸如 sigaddset(&sa.sa_mask,SIGUSR2);这样的语句来针对信号为SIGUSR1时做处理
        // 这里.sa_mask是一个信号集（描述信号合集），用于表示要阻塞的信号，sigemptyset()这个函数：把信号集中所有的信号清0，本意就是不准备阻塞任何信号
        


        // 设置信号处理动作（信号处理函数—）。说白了就是这里就是让信号来了后，调用我的处理程序，有个旧的同类函数 叫 signal， 不过signal这个函数被认为是不可靠信号语义，不建议使用
        // 参数1：要操作的信号
        // 参数2：主要就是那个信号处理函数以及执行信号处理函数时要屏蔽的信号等等内容
        // 参数3：返回已往的对信号的处理方式【跟sigprocmask()函数右边第三个参数是一样的】,跟参数2是同一个类型，这里不需要这个东西，所以直接设置为NULL
        if(sigaction(sig->signo, &sa, NULL) == -1)
        {
            ngx_log_error_core(NGX_LOG_EMERG, errno, "sigactoin(%s) faild", sig->signame);  // 显示到日志文件中去
            return -1;  // 有失败就直接返回
        }
        else
        {
            // ngx_log_error_core(NGX_LOG_EMERG,errno,"sigaction(%s) succed!",sig->signame);     //成功不用写日志 
            ngx_log_stderr(0, "sigaction(%s) success!", sig->signame);  // 直接往屏幕上打印，不需要时间可以去掉
        }
    }
    return 0;
    
}

//信号处理函数
static void ngx_signal_handler(int signo, siginfo_t *siginfo, void *ucontext)
{
    printf("来信号了\n");
}






