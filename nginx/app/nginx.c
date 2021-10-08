#include <stdio.h>
#include <unistd.h>

#include "ngx_func.h"   // 头文件路径，已经使用gcc -I 参数指定了
#include "ngx_signal.h"

int main(int argc, char *const *argv)
{
    printf("linux通讯架构实战\n");
    myconf();
    mysignal();

    // for(;;)
    // {
    //     sleep(1);
    //     printf("休息1秒\n");
    // }
    printf("程序退出！\n");
    return 0;
}