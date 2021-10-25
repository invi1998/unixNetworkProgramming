#ifndef __NGX_COMM_H__
#define __NGX_COMM_H__

// 宏定义-----------------------------------------------------
#define __PKG_MAX_LENGTH__      30000       // 每个包的最大长度【包体+包头】不超过这个数字，为了留出一些空间
                                            // 实际上编码是，包头+包体长度必须不大于这个值-1000 【也就是 29000】

// 通信 收包状态定义
#define _PKG_HD_INIT            0           // 初始状态，准备接收数据包头
#define _PKG_HD_RECVING         1           // 接收包头中，包头不完整，继续接收中
#define _PKG_BD_INIT            2           // 包头刚好接收完，准备接收包体
#define _PKG_BD_RECVING         3           // 接收包体中，包体不完整，继续接收中，处理后直接回到_PKG_HD_INIT状态
// #define _PKG_RV_FINISHED        4           // 完整包收完，这个状态似乎没什么用

#define _DATA_BUFSIZE_          20          // 因为我要先收包头，我希望定义一个固定大小的数组，专门用来收包头，这个数字大小一定要 > sizeof(COMM_PKG_HEADER)， 所以这里定义为20
                                            // 如果日后COMM_PKG_HEADER大小变动，这里的数字也要做出调整以满足 > sizeof(COMM_PKG_HEADER)的要求

// 结构定义----------------------------------------------------
#pragma pack(1)    // 对齐方式，1字节对齐【结构之间成员不做任何字节对齐：紧密的排列在一起】

// 一些和网络通讯相关的结构放在这里
// 包头结构
typedef struct _COMM_PKG_HEADER
{
    unsigned short pkgLen;      // 报文总长度【包体+包头】 --- 2字节。 2字节可以表示的最大数字为6万多，我们可以定义_PKG_MAX_LENGTH 30000; 所以这里用pkgLen足够保存下
                                // 包头中记录着整个包【包体+包头】的长度

    unsigned short msgCode;     // 消息类型代码--2字节，用于区别每个不同的命令【不同的消息】
    int            crc32;       // CRC32校验--4字节；为了反正收发数据中出现收到的内容和发送的内容不一致的情况，引入这个字段做一个基本校验使用

}COMM_PKG_HEADER,*LPCOMM_PKG_HEADER;

#pragma pack()      // 取消指定对齐，恢复缺省对齐

#endif