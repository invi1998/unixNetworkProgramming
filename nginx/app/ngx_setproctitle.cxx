#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> // env
#include <string.h>

#include "ngx_global.h"

// 设置可执行程序标题相关函数：分配内存，并且把环境变量拷贝到新内存中来
void ngx_init_setproctitle()
{

    // 这里无需判断 penvmen == NULL,有些编译器new会返回NULL，有些会爆异常，但是不管怎么样，
    // 如果在重要的地方new失败了，你无法收场，让程序崩溃，帮助你发现问题为好
    gp_envmem = new char[g_envneedmem];
    memset(gp_envmem, 0, g_envneedmem);     // 内存要清空防止出现问题

    char *ptmp = gp_envmem;

    // 把原来的内存内容般到新地方
    for(int i = 0; environ[i]; i++)
    {
        size_t size = strlen(environ[i])+1;
        // 不要落下+1；否者内存全乱套了。因为strlen是不包括字符串末尾符\0的
        strcpy(ptmp, environ[i]);
        // 把原环境变量内容拷贝到新内存中
        environ[i] = ptmp;
        ptmp += size;
    }
    return;
}

// 设置可执行程序标题
void ngx_setproctitle(const char * title)
{

    // 这里我们假设，所有的命令行 参数我们都不要了，可以被随意覆盖；
    // 注意：我们标题的长度，不会长到原始标题和原始环境变量都装不下，否者怕出问题，不处理

    // 1）计算新标题的长度
    size_t ititlelen = strlen(title);

    // size_t e_environlen = 0;    // e 表示全局变量
    // for(int i = 0; g_os_argv[i]; i++)
    // {
    //     e_environlen += strlen(g_os_argv[i])+1;
    // }

    // 2）计算总的原始的argv的那块内存的总长度【包括各种参数】
    size_t esy = g_argvneedmem + g_envneedmem;   // argv和environ内存总和

    if(esy <= ititlelen)
    {
        // 标题过长，argv和environ总和都存不下。
        // 注意字符串末尾多了一个 \0 ，所以这块的判断是 <= 【也就是=都算存不下】
        return;
    }

    // 空间够保存标题

    // 3）设置后续的命令行参数为空，表示只有argv[]中一个元素，这是好习惯，防止后续argv被滥用，
    // 因为很多判断是用argv[] == NULL来做结束标记判断的;
    g_os_argv[1] = NULL;

    // 4) 把标题弄进来，注意原来的命令行参数都会被覆盖掉，不要在使用这些命令行参数，而且g_os_argv[1]已经被置空
    char *ptmp = g_os_argv[0];
    // 让ptmp指向g_os_argv所指向的内存
    strcpy(ptmp,title);
    ptmp += ititlelen; // 跳过标题长度

    // 5）把剩下的原argv以及environ所占的内存全部清零，否者会出现ps的cmd列可能还会残余一些没有被覆盖的内容
    size_t cha = esy - ititlelen;
    // 内存总和减去标题字符串长度（不包含字符串末尾的\0），剩余的大小，就是要memset的
    memset(ptmp, 0, cha);
    return;
    

}