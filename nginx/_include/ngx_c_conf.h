#ifndef __NGX_CONF_H__
#define __NGX_CONF_H__

#include <vector>

#include "ngx_global.h"
// 一些全局的、通用的定义

// 类名可以遵照一定的命名规范，比如这里就是：第一个字母是C，后续的单词首字母大写
class CConfig
{
    private:
        CConfig();
    public:
        ~CConfig();
    private:
        static CConfig *m_instance;

    public:
        static CConfig* GetInstance()
        {
            if(m_instance == NULL)
            {
                // 锁
                if(m_instance == NULL)
                {
                    m_instance = new CConfig();
                    static CGarhuisou cl;
                }
                // 放锁
            }
            return m_instance;
        }
        class CGarhuisou    // 类中套类，用于释放对象
        {
            public:
                ~CGarhuisou()
                {
                    if(CConfig::m_instance)
                    {
                        delete CConfig::m_instance;
                        CConfig::m_instance = NULL;
                    }
                }
        };

// ----------------------------------
    
    public:
        bool Load(const char *pconfName); // 装载配置文件
        const char* GetString(const char *p_itemname);
        int GetIntDefault(const char *p_itemname, const int def);

    public:
        std::vector<LPCConfItem> m_ConfigItemList;  // 存储配置信息的列表

};

#endif