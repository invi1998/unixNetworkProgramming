#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string>

#include "ngx_c_conf.h"     // 和配置文件处理相关的类，名字带C_表示和类有关
#include "ngx_func.h"   // 头文件路径，已经使用gcc -I 参数指定了 各种函数声明
#include "ngx_signal.h"

// 和设置标题有关的全局量
char **g_os_argv;   // 原始命令行参数数组，在main中会被赋值
char *gp_envmem = NULL; // 指向自己分配的env环境变量的内存
int g_environlen = 0;   // 环境变量所占内存的大小

int main(int argc, char *const *argv)
{
    
    g_os_argv = (char **) argv;
    ngx_init_setproctitle();    // 把环境变量进行搬家

    // 我们在main中，先把配置文件读取出来，然后供后续使用
    CConfig *p_config = CConfig::GetInstance(); // 单例类
    if(p_config->Load("nginx.conf") == flase)
    {
        printf("配置文件载入失败！程序退出！\n");
        exit(1);
    }

    return 0;
}