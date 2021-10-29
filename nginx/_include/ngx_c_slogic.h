
#ifndef __NGX_C_SLOGIC_H__
#define __NGX_C_SLOGIC_H__

#include <sys/socket.h>
#include "ngx_c_socket.h"

// 处理逻辑和通讯的子类
class CLogicSocket : public CSocket // 继承自父类CSocket
{
    
public:
    CLogicSocket();                     // 构造函数
    virtual ~CLogicSocket();            // 析构函数
    virtual bool Initialize();          // 初始化函数

public:
    // 各种业务逻辑相关的函数都在这里
    bool _HandleRegister(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgBody, unsigned short iBodyLength);
    bool _HandleLogIn(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgBody, unsigned short iBodyLength);

public:
    virtual void threadRecvProcFunc(char *pMsgBuf);


};


#endif