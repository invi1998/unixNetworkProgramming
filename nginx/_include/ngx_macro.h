#ifndef __NGX_MACRO_H__
#define __NGX_MACRO_H__

// 各种define宏定义相关的定义放在这里

#define NGX_MAX_ERROR_STR 2048  // 显示错误信息的最大数组长度

// 简单功能函数--------------------------------------------
// 类似memcpy,但是常规的memcpy返回的是指向目标dst的指针，而这里这个ngx_cpymem返回的是目标【拷贝数据后】的终点位置
// 连续复制多段数据时比较方便
#define ngx_cpymem(dst,src,n)   (((u_char *)memcpy(dst, src, n)) + (n))
// 注意#define的写法，n这里用()包裹防止出现什么错误
#define ngx_min(val1, val2)     ((val1 > val2) ? (val1) : (val2))
// 比较大小，返回小值，注意使用()包裹


// 数字相关---------------------------------------------------
#define NGX_MAX_UINT32_VALUE    (uint32_t) 0xffffffff   // 最大的32位无符号数：十进制是4294967295
#define NGX_INI64_LEN           (sizeof("-9223372036854775808") - 1)



// 日志相关-------------------------------------------------
// 这里把日志一共分为八个等级【级别从高到底，数字最小的级别最高，数字大的级别最低】
// 方便管理，显示，过滤等

#define NGX_LOG_STDERR            0    //控制台错误【stderr】：最高级别日志，日志的内容不再写入log参数指定的文件，而是会直接将日志输出到标准错误设备比如控制台屏幕
#define NGX_LOG_EMERG             1    //紧急 【emerg】
#define NGX_LOG_ALERT             2    //警戒 【alert】
#define NGX_LOG_CRIT              3    //严重 【crit】
#define NGX_LOG_ERR               4    //错误 【error】：属于常用级别
#define NGX_LOG_WARN              5    //警告 【warn】：属于常用级别
#define NGX_LOG_NOTICE            6    //注意 【notice】
#define NGX_LOG_INFO              7    //信息 【info】
#define NGX_LOG_DEBUG             8    //调试 【debug】：最低级别

#define NGX_ERROR_LOG_PATH       "logs/error1.log"   //定义日志存放的路径和文件名 

#endif