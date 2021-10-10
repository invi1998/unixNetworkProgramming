
// 系统头文件放最上面

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// 自定义的头文件放下面，因为g++中用了 -I 参数，所以这里使用<>也可以
#include "ngx_func.h"   // 函数声明
#include "ngx_c_conf.h" // 和配置文件处理相关的类，名字带C_的表示和类有关

// 静态成员赋值
CConfig *CConfig::m_instance = NULL;

// 构造函数
CConfig::CConfig()
{

}


// 析构函数
CConfig::~CConfig()
{
    std::vector<LPCConfItem>::iterator pos;
    for(pos = m_ConfigItemList.begin(); pos!=m_ConfigItemList.end(); ++pos)
    {
        delete (*pos);
    }
    m_ConfigItemList.clear();
}

// 装载配置文件
bool CConfig::Load(const char *pconfName)
{
    FILE *fp;
    fp = fopen(pconfName, "r");
    if(fp == NULL)
        return false;

    // 每一行配置文件都读出来放在这里
    char linebuf[501];  //  每行的配置都不要太长，保持 < 500 字符，防止出现问题

    // 走到这里，文件打开成功
    while(!feof(fp))    // 检查文件是否结束，没有结束则条件成立
    {
        if(fgets(linebuf, 500, fp) == NULL) // 从文件中读数据，每次读一行，一行最多不要超过500个字符
            continue;
        
        if(linebuf[0] == 0)
            continue;

        // 处理注释行
        if(*linebuf==';' || *linebuf==' ' || *linebuf=='#' || *linebuf=='\t'||*linebuf=='\n')
            continue;
        
    lblprocstring:
        // 屁股后面若有换行，回车，空格等都截取掉
        if(strlen(linebuf)>0)
        {
            if(linebuf[strlen(linebuf)-1] == 10 || linebuf[strlen(linebuf)-1]==13||linebuf[strlen(linebuf)-1]==32)
            {
                linebuf[strlen(linebuf)-1]=0;
                goto lblprocstring;
            }
        }
        if(linebuf[0] == 0)
            continue;
        if(*linebuf=='[')   // [开头的也不处理
            continue;

        // 这种 "ListenPort = 5678" 走下来
        char *ptmp = strchr(linebuf, '=');
        if(ptmp != NULL)
        {
            LPCConfItem p_confitem = new CConfItem;
            // 注意前面带LP（指针），后面new这里的类型不带（结构）
            // 其实就是 int *p = new int;
            memset(p_confitem,0,sizeof(CConfItem));
            // 将p_confitem指向的内存全部设置为0
            strncpy(p_confitem->ItemName,linebuf,(int)(ptmp->linebuf));
            // 等号左侧的拷贝到p_confitem->ItemName (也就是配置项左边的名字)
            strncpy(p_confitem->ItemContent,ptmp+1);
            // 等号右侧的拷贝到p_confitem->ItemContent (也就是配置项右边的值)

            // 下面这个是将配置项前后的空格都去掉
            Rtrim(p_confitem->ItemName);
            Ltrim(p_confitem->ItemName);
            Rtrim(p_confitem->ItemContent);
            Ltrim(p_confitem->ItemContent);

            // printf("itemname=%s | itemcontent=%s\n", p_confitem->ItemName, p_confitem->ItemContent);
            m_ConfigItemList.push_back(p_confitem);
            // 内存要释放，因为这里是new出来的


        } // end if
    } // end while

    fclose(fp); // 关闭文件，这一步不能忘记
    return true;
}


// 根据ItemName获取配置信息字符串，不修改不用互斥
const char* CConfig::GetString(const char* p_itemname)
{
    std::vector<LPCConfItem>::iterator pos;
    for(pos = m_ConfigItemList.begin();pos!=m_ConfigItemList.end();++pos)
    {
        if(strcasecmp((*pos)->ItemName,p_itemname)==0)
            return (*pos)->ItemContent;
    }   // end if
    return NULL;
}

// 根据ItemName获取数字类型配置信息，不修改不用互斥
int CConfig::GetIntDefault(const char *p_itemname, const int def)
{
    std::vector<LPCConfItem>::iterator pos;
    for(pos=m_ConfigItemList.begin(); pos!=m_ConfigItemList.end();++pos)
    {
        if(strcasecmp((*pos)->ItemName,p_itemname)==0)
            return atoi((*pos)->ItemContent);
    }   // end for
    return def;
}

