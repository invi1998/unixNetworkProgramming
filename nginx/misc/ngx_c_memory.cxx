// 和内存分配有关的函数放在这里
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_memory.h"

// 类静态成员赋值
CMemory *CMemory::m_instance = NULL;

// 分配内存
// memsCount: 分配的字节大小
// ifmemset：是否要把分配的内存初始化为0
void *CMemory::AllocMemory(int memCount, bool ifmemset)
{
    void *temDate = (void *)new char[memCount];     // 不对new是否成功进行判断，如果new失败，程序就不应该继续运行，就让他崩溃以方便我们排错
    if (ifmemset)
    {
        memset(temDate, 0, memCount);
    }
    return temDate;
    
}

// 内存释放函数
void CMemory::FreeMemory(void *point)
{
    // delete []point;     // 这么删除编译会爆警告： warning: deleting 'void*' is undefined [-Wdelete-incomplete]
    // 如果我们new出来的指针是一个基本类型，没什么关系，内存还是会被释放的，但是如果是一个类对象指针，在处理过程中转成了void*，那就有问题了，析构函数将不会被调用。
    // 故new的指针类型要和delete的指针类型要保持一致。

    // 如下所示：
    // object* p=new object[10];

    // void* p2=(void *)p;

    // //注意指针转换
    // delete[] (object*) p;

    delete [] ((char *)point);  // new 的时候是char *,这里弄回char *，以免出现警告
}