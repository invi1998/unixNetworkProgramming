// 和开启子进程有关

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>         //  信号相关头文件
#include <errno.h>          // errno

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"


// 函数声明
static void ngx_start_worker_processes(int threadnums);
static void ngx_spawn_process(int threadnums.const char *pprocname);
static void ngx_worker_process_cycle(int inum, const char *pprocname);
static void ngx_worker_process_init(int inum);

// 变量声明
static u_char master_process[] = "master process";

// 描述：创建worker子进程
void ngx_master_process_cycle()
{
    sigset_t set;               // 信号集

    sigemptyset(&set);          // 清空信号集

    // 下列这些信号在执行本函数期间不希望收到【考虑到官方nginx中有这些信号】(保护不希望有信号中断的代码临界区)
    // 建议fork()子进程时学习这种写法，防止信号干扰
    sigaddset(&set, SIGCHLD);       // 子进程状态改变
    sigaddset(&set, SIGALRM);       // 定时器超时
    sigaddset(&set, SIGIO);         // 异步IO
    sigaddset(&set, SIGINT);        // 终端中断符
    sigaddset(&set, SIGHUP);        // 连接断开
    sigaddset(&set, SIGUSR1);       // 用户定义信号
    sigaddset(&set, SIGUSR2);       // 用户定义信号
    sigaddset(&set, SIGWINCH);      // 终端窗口大小改变
    sigaddset(&set, SIGTERM);       // 终止
    sigaddset(&set, SIGQUIT);       // 终端退出符

    // 可以根据实际开发需要往其中添加其他需要屏蔽的信号

    // 设置，此时无法接受的信号；阻塞期间，你发过来的上述信号，多个会被合并为一个，暂存者，等你放开信号屏蔽之后才能收到这些信号
    // sigprocmask()

// 
// C 库函数 - strcat()
// C 标准库 - <string.h>

// 描述
// C 库函数 char *strcat(char *dest, const char *src) 把 src 所指向的字符串追加到 dest 所指向的字符串的结尾。

// 声明
// 下面是 strcat() 函数的声明。

// char *strcat(char *dest, const char *src)
// 参数
// dest -- 指向目标数组，该数组包含了一个 C 字符串，且足够容纳追加后的字符串。
// src -- 指向要追加的字符串，该字符串不会覆盖目标字符串。
// 返回值
// 该函数返回一个指向最终的目标字符串 dest 的指针。

// 第一个参数用了SIG_BLOCK表明设置 进程 新的信号屏蔽字为 “当前信号屏蔽字 和 第二个参数指向的信号集的 并集”
    if(sigprocmask(SIG_BLOCK, &set, NULL) == -1)
    {
        ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_master_process_cycle()中sigprocmask()失败！");
    }

    // 即便sigprocmask()失败，程序流程也要继续往下走
    
    // 首先 设置主进程标题  ---------------------------------begin
    size_t size;
    int i;
    size = sizeof(master_process);          // 注意这里用的是sizeof，所以字符串末尾的\0是被计算进去的
    size += g_argvneedmem;                  // argv参数长度加进来
    if(size < 1000)                         // 长度小于这个，才设置标题
    {
        char title[1000] = {0};
        strcpy(title, (const char *)master_process);    // "master process"
        strcat(title, " ");                 // 跟一个空格分开，会更清晰些 "master process "
        for (i = 0; i < g_os_argc; i++)
        {
            strcat(title, g_os_argv[i]);
        }
        ngx_setproctitle(title);
    }
    // 首先先设置主进程的标题 -------------------end


    // 从配置文件中读取想要创建的worker进程数量
    CConfig *p_config = CConfig::GetInstance();     // 单例类
    int wokerprocess = p_config->GetIntDefault("WorkerProcess", 1);     // 从配置文件中获得想要的worker进程数量
    ngx_start_worker_processes(wokerprocess);       // 这里要创建子进程

    // 创建子进程后，父进程的执行流程会返回这里，子进程不会走进来
    sigemptyset(&set);      // 信号屏蔽字为空，表示不屏蔽任何信号

    for(;;)
    {
        // usleep(100000);
        ngx_log_error_core(9, 0, "打印父进程， pid为%p", ngx_pid);

        // 1）根据给定的参数设置新的mask 并 阻塞当前进程 【因为是一个空集，所以不会阻塞任何信号】
        // 2）此时，一旦收到信号，便恢复原先的信号屏蔽【因为原来的mask在上面设置的，阻塞了多达10个信号，从未保证下面的执行流程不会再被其他信号截断】
        // 3）调用该信号对应的信号处理函数
        // 4）信号处理函数返回之后，sigsuspend返回，使得流程继续往下走
        // printf("for 进来了！\n");        // 发现，如果printf不加 \n ，无法及时显示到屏幕上去，是行缓存的问题，


        // sigsuspend(&set);   // 阻塞在这里，等待一个信号，此时进程是挂起的，不占用CPU时间，只有收到信号才会被唤醒（返回）
        //                     // 此时master进程完全靠信号驱动干活
        
        // printf("执行到sigsuspend下面来了\n");

        // printf("master休息1秒\n");
        // sleep(1);
        // 后续扩充
    }

    return;

}

// 描述：根据给定参数创建指定数量的子进程，因为以后可能要扩展功能，增加参数，所以单独写成一个函数
// threadnums:要创建的子进程数量
static void ngx_start_worker_processes(int threadnums)
{
    int i;
    for(i = 0; i < threadnums; i++)
    {
        ngx_spawn_process(i, "worker process");
    }
    return;
}

// 描述：产生一个子进程
// inum：进程编号【0】
// pprocename: 子进程名字 - worker process
static int ngx_spawn_process(int inum, const char *pprocname)
{
    pid_t pid;

    pid = fork();   // fork()系统调用产生子进程
    switch (pid)
    {
    case -1:    // 产生子进程失败
        ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_spawn_process()fork()子进程num=%d,procname=\"%s\"失败！", inum, pprocname);
        return -1;
    
    case 0:     // 子进程分支
        ngx_parent = ngx_pid;                       // 因为是子进程了，所有原来的pid都变成了父PID
        ngx_pid = getpid();                         // 重新获取pid，即本子进程的pid
        ngx_worker_process_cycle(inum, pprocname);  // 希望所有的worker子进程，都在这个函数里不断循环，不出来，也就是说，子进程不往下走
        break;
    
    default:    // 这个应该是父进程分子，直接break，流程往switch之后走
        break;
    }

    // 父进程分支会走到这里，子进程流程不往下走
    // 如有需要，可扩展其他代码
    return pid;

}

// 描述：worker子进程的功能函数，每个woker子进程，都在这里循环，（无限循环【处理网络事件和定时器事件 以对外提供服务】）
// 子进程才会走到这里面
// inum：进程编号【从0开始】
static void ngx_worker_process_cycle(int inum, const char *pprocname)
{
    // 这一步调用 ngx_worker_process_init主要是将屏蔽了的信号集释放出来（不然子进程一直保持信号屏蔽，将会接收不到父进程或者其他进程发送的信号）
    ngx_worker_process_init(inum);
    // 重新为子进程设置进程名，不要与父进程重复-------------------------
    ngx_setproctitle(pprocname);        // 设置标题

    // 暂时先放个死循环
    // setvbuf(stdout, NULL, _IONBF, 0);    // 这个函数，直接将printf缓冲区禁止了，printf就直接输出了
    for(;;)
    {
        // 先sleep，以后扩充
        //printf("worker进程休息1秒");       
        //fflush(stdout); //刷新标准输出缓冲区，把输出缓冲区里的东西打印到标准输出设备上，则printf里的东西会立即输出；
        //sleep(1); //休息1秒       
        //usleep(100000);
        ngx_log_error_core(0,0,"good--这是子进程，编号为%d,pid为%P！",inum,ngx_pid);
        //printf("1212");
        //if(inum == 1)
        //{
            //ngx_log_stderr(0,"good--这是子进程，编号为%d,pid为%P",inum,ngx_pid); 
            //printf("good--这是子进程，编号为%d,pid为%d\r\n",inum,ngx_pid);
            //ngx_log_error_core(0,0,"good--这是子进程，编号为%d",inum,ngx_pid);
            //printf("我的测试哈inum=%d",inum++);
            //fflush(stdout);
        //}
            
        //ngx_log_stderr(0,"good--这是子进程，pid为%P",ngx_pid); 
        //ngx_log_error_core(0,0,"good--这是子进程，编号为%d,pid为%P",inum,ngx_pid);
    }
    return;
}

// 描述：子进程创建时调用本函数进行一些初始化工作
static void ngx_worker_process_init(int inum)
{
    sigset_t set;               // 信号集

    sigemptyset(&set);          // 清空信号集
    if(sigprocemask(SIG_SETMASK, &set, NULL) == -1)     // 原来是屏蔽那10个信号，【防止fork()期间受到信号导致混乱】现在不在屏蔽任何信号
    {
        ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_worker_process_init()中sigprocmask()失败！");
    }

    // 后续扩展

    return;

}