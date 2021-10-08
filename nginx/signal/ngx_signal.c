#include <stdio.h>
#include <unistd.h>

void mysignal()
{
    printf("执行了mysignal函数\n");
    return;
}