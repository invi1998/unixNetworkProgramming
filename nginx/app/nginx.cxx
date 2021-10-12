#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_c_conf.h"     // 和配置文件处理相关的类，名字带C_表示和类有关
#include "ngx_func.h"       // 头文件路径，已经使用gcc -I 参数指定了 各种函数声明
#include "ngx_signal.h"

// 本文件用的函数声明
static void freeresource();

// 和设置标题有关的全局量
size_t g_argvneddmem=0;             // 保存下这些argv参数所需的内存大小
size_t g_envneedmem=0;              // 保存环境变量所需的内存大小
int g_os_argc;                      // 参数个数
char **g_os_argv;                   // 原始命令行参数数组，在main中会被赋值
char *gp_envmem = NULL;             // 指向自己分配的env环境变量的内存，在ngx_init_setproctitile()函数中会被分配内存

//和进程本身有关的全局量
pid_t ngx_pid;                  //当前进程的pid
pid_t ngx_parent;               // 父进程的pid


int main(int argc, char *const *argv)
{   
    int exitcode = 0;           //退出代码，先给0表示正常退出
    int i = 0;                  // 临时使用

    //(1)无伤大雅也不需要释放的放最上边    
    ngx_pid = getpid();         //取得进程pid
    ngx_parent = getppid();     // 取得父进程id
    g_os_argv = (char **) argv; //保存参数指针    

    // 统计argv所占用的内存
    g_argvneddmem = 0;

    for (i = 0; i < argc; i++)  // argv = ./nginx -a -b -c asgsd
    {
        g_argvneddmem += strlen(argv[i]) + 1;   // +1是留给 \0 的空间
    }
    
    // 统计环境变量所占的内存。注意判断方法是environ[i]是否作为环境变量结束标记
    for(i = 0; environ[i]; i++)
    {
        g_envneedmem += strlen(environ[i]) + 1; // +1实际上是因为末尾有\0，是占据实际内存位置的，要算进来
    }

    g_os_argc = argc;           // 保存参数个数
    g_os_argv = (char **) argv; // 保存参数指针

    //(2)初始化失败，就要直接退出的
    //配置文件必须最先要，后边初始化啥的都用，所以先把配置读出来，供后续使用 
    CConfig *p_config = CConfig::GetInstance(); //单例类
    if(p_config->Load("nginx.conf") == false) //把配置文件内容载入到内存        
    {        
        ngx_log_stderr(0,"配置文件[%s]载入失败，退出!","nginx.conf");
        //exit(1);终止进程，在main中出现和return效果一样 ,exit(0)表示程序正常, exit(1)/exit(-1)表示程序异常退出，exit(2)表示表示系统找不到指定的文件
        exitcode = 2; //标记找不到文件
        goto lblexit;
    }
    
    //(3)一些初始化函数，准备放这里
    ngx_log_init();             //日志初始化(创建/打开日志文件)


    //(4)一些不好归类的其他类别的代码，准备放这里
    ngx_init_setproctitle();    //把环境变量搬家

    //(5)开始正式工作流程，主流程一直在下面这个函数里面循环，暂时不会走下去，资源释放这些后续完善
    ngx_master_process_cycle();

    
    //--------------------------------------------------------------    
    for(;;)
    //for(int i = 0; i < 10;++i)
    {
        sleep(1); //休息1秒        
        printf("休息1秒\n");        

    }
      
    //--------------------------------------
lblexit:
    //(5)该释放的资源要释放掉
    freeresource();  //一系列的main返回前的释放动作函数
    printf("程序退出，再见!\n");
    return exitcode;
}

//专门在程序执行末尾释放资源的函数【一系列的main返回前的释放动作函数】
void freeresource()
{
    //(1)对于因为设置可执行程序标题导致的环境变量分配的内存，我们应该释放
    if(gp_envmem)
    {
        delete []gp_envmem;
        gp_envmem = NULL;
    }

    //(2)关闭日志文件
    if(ngx_log.fd != STDERR_FILENO && ngx_log.fd != -1)  
    {        
        close(ngx_log.fd); //不用判断结果了
        ngx_log.fd = -1; //标记下，防止被再次close吧        
    }
}
