// 和日志相关的函数放在这里

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>     // uintptr_t
#include <stdarg.h>     // va_start...
#include <unistd.h>     // STDERR_FILENO等
#include <sys/timeb.h>  // gettimeofday
#include <time.h>       // localtime_r
#include <fcntl.h>      // open
#include <errno.h>      // errno


#include "ngx_global.h"
#include "ngx_macro.h"
#include "ngx_func.h"
#include "ngx_c_conf.h"


// 全局量-------------------------------------------------
// 错误等级，和ngx_macro.h里定义的日志等级宏是一一对应的
static u_char err_levels[][20] =
{
    {"stderr"},    //0：控制台错误
    {"emerg"},     //1：紧急
    {"alert"},     //2：警戒
    {"crit"},      //3：严重
    {"error"},     //4：错误
    {"warn"},      //5：警告
    {"notice"},    //6：注意
    {"info"},      //7：信息
    {"debug"}      //8：调试
};
ngx_log_t ngx_log;

// --------------------------------------------------------------------------------------------------
// 描述：通过可变参数组合出字符串【支持...省略号形参】，自动往字符串最末尾增加换行符
// 所以调用者不需要加\n，往标准错误上输出这个字符串
// 如果err不为0,表示有错误，会将该错误编号以及对应的错误信息一并放到组合出的字符串中一起显示

// 比较典型的C语言写法：就是这种va_start,va_end
// fmt:通过这第一个普通参数来寻址到后续的所有可变参数的类型及其值
// 调用格式比如：ngx_log_stderr(0, "invalid option: \"%s\", %d", "testinfo", 123);

// ngx_log_stderr(0, "invalid option: \"%s\"", argv[0]);  //nginx: invalid option: "./nginx"
// ngx_log_stderr(0, "invalid option: %10d", 21);         //nginx: invalid option:         21  ---21前面有8个空格
// ngx_log_stderr(0, "invalid option: %.6f", 21.378);     //nginx: invalid option: 21.378000   ---%.这种只跟f配合有效，往末尾填充0
// ngx_log_stderr(0, "invalid option: %.6f", 12.999);     //nginx: invalid option: 12.999000
// ngx_log_stderr(0, "invalid option: %.2f", 12.999);     //nginx: invalid option: 13.00
// ngx_log_stderr(0, "invalid option: %xd", 1678);        //nginx: invalid option: 68E
// ngx_log_stderr(0, "invalid option: %Xd", 1678);        //nginx: invalid option: 68E
// ngx_log_stderr(15, "invalid option: %s , %d", "testInfo",326);        //nginx: invalid option: testInfo , 326
// ngx_log_stderr(0, "invalid option: %d", 1678); 

// 往屏幕上打印一条错误信息
void ngx_log_stderr(int err, const char *fmt, ...)
{
    va_list args;
    // 创建一个va_list数据类型变量
    u_char errstr[NGX_MAX_ERROR_STR+1];
    // 2048 -- *************** +1 感觉官方写法有点瑕疵
    u_char *p, *last;

    memset(errstr, 0, sizeof(errstr));
    // 这块有必要加，至少在va_end之前有必要，否者字符串没有结束标记是不行的

    last = errstr + NGX_MAX_ERROR_STR;
    // last指向的是整个buffer最后，【指向最后一个有效位置的后面也就是非有效位】，作为一个标记，
    // 其实就是标记，只要你字符串长度不要超出这个last，那就说明是安全的，没有越界
    // 防止输出内容超出这么长
    // 这里认为是有问题的，所以才在上面 u_char errstr[NGX_MAX_ERROR_STR+1]; 给了加1
    // 比如你定义了char tmp[2]，你如果last = tmp+2，那么last实际上指向了tmp[2]，而tmp[2]在使用中是无效的

    p = ngx_cpymem(errstr,"nginx: ", 7);    // p指向“nginx: ”之后
    // 这里为什么是指向“nginx: ”之后，注意去看 ngx_cpymem 的定义

    //    va_start(ap,fmt);//将第一个可变参数的地址付给ap，即ap指向可变参数列表的开始
        //ch = va_arg( ap, char *);//取出ap里面的值，即第一个可变参数，char *根据实际传入的参数类型改变，调用va_arg后ap自增；
        //ch1 = va_arg( ap, char);//取出ap里面的值，即第二个可变参数，char需根据可变参数具体类型改变，调用va_arg后ap自增；
        //i = va_arg( ap, int );//取出ap里面的值，即第三个可变参数，int需根据可变参数具体类型改变，调用va_arg后ap自增；
    //     vsprintf(string,fmt,ap);//将参数fmt、ap指向的可变参数一起转换成格式化字符串，放string数组中，具体自行百度vsprintf相关功能
    //     va_end(ap); //ap付值为0，没什么实际用处，主要是为程序健壮性
        //putchar(ch); //打印取出来的字符
    //     printf(string); //把格式化字符串打印出来

    va_start(args,fmt); // 使用args指向起始的参数（可变参数就这么用）
    p = ngx_vslprintf(p, last,fmt, args);   // 组合出这个字符串保存在errstr中
    va_end(args);   // 释放args

    if(err) // 如果错误代码不是0,表示有错误发生
    {
        // 错误代码和错误信息也要显示出来
        p = ngx_log_errno(p, last, err);

    }

    // 若位置不够，那换行也要硬插到末尾，哪怕覆盖到其他内容
    if(p>= (last -1))
    {
        p = (last - 1)-1;
        // 把尾部空格留出来，这里感觉nginx的处理有点问题
        // last-1，才是最后一个有效的内存，而这个位置要保存\0，所以这里需要再-1，这个位置才适合保存\n
    }

    *p++ = '\n';    // 增加换行符

    // 往标准错误【一般是屏幕】输出信息
    write(STDERR_FILENO, errstr, p-errstr);

    // 往标准错误中写的内容，为了防止看不到，会同时打印一份相同的信息到日志文件中
    //如果这是个有效的日志文件，本条件肯定成立，此时也才有意义将这个信息写到日志文件
    if (ngx_log.fd > STDERR_FILENO)
    {
        ngx_log_error_core(NGX_LOG_STDERR,err,(const char *)errstr);
        //这里有个\n，ngx_log_error_core还有个\n，所以写到日志会有一个空行多出来
    }
    

    // 测试代码
    //printf("ngx_log_stderr()处理结果=%s\n",errstr);
    //printf("ngx_log_stderr()处理结果=%s",errstr);

    return;

}

// ----------------------------------------------------------------------------------------------------------------------
// 描述：给一段内存，一个错误编号，这里需要组合出一个字符串，形如：（错误编号：错误原因），放到这段内存中去
//
// buf：是一个内存，要往这里保存数据
// last：放的数据不要超过这里
// err：错误编号，这里是要取的这错误编号对应的错误字符串，保存到buffer中
u_char *ngx_log_errno(u_char *buf, u_char *last, int err)
{
    char *perrorinfo = strerror(err);
    // 根据资料不会放回NULL
    size_t len = strlen(perrorinfo);

    // 这里插入一些字符串：（%d）
    char leftstr[10] = {0};
    sprintf(leftstr," (%d: ", err);
    size_t leftlen = strlen(leftstr);

    char rightstr[] = ") ";
    size_t rightlen = strlen(rightstr);

    size_t extralen = leftlen + rightlen;   // 左右额外的宽度
    if((buf+len+extralen) < last)
    {
        // 这里需要保证整个都能装下才装，否者就全部抛弃，nginx的做法是，如果位置不够，就硬留出50个位置【哪怕会覆盖掉已往的有效内容】，也要硬往后面塞，当然这样也可以
        buf = ngx_cpymem(buf, leftstr, leftlen);
        buf = ngx_cpymem(buf, perrorinfo, len);
        buf = ngx_cpymem(buf, rightstr,rightlen);
    }

    return buf;
}

// --------------------------------------------------------------------------------------------------------------------------------------
// 往文件中写日志，代码中有自动加换行符，所以调用时字符串不用刻意加\n
// 日志定位标准错误，则直接往屏幕上写日志【比如日志文件打不开，这回直接定位到标准错误，此时日志就打印到屏幕上，参考 ngx_log_init()】
// 
// level：一个等级数字，如果我们把日志分为一些等级，已方便管理，显示，过滤等，如果这个等级数字比配置文件中的等级数字“LogLevel”大，那么这条信息就不会被写入到日志文件中
// err: 是个错误代码，如果不是0，就应该转换为显示对应的错误信息，一起写入到日志文件中
// ngx_log_error_core(5,7,"这个xxx工作空间有问题，显示的结果是= %s", "yyyyyyyyyy");

void ngx_log_error_core(int level, int err, const char *fmt, ...)
{
    u_char *last;
    u_char errstr[NGX_MAX_ERROR_STR+1];
    // 这个+1 可以参考ngx_log_stderr()函数的写法

    memset(errstr, 0, sizeof(errstr));
    last = errstr + NGX_MAX_ERROR_STR;

    struct timeval  tv;
    struct tm       tm;
    time_t          sec;    //  秒
    u_char          *p;     // 指向当前要拷贝数据到其中的内存位置
    va_list         args;

    memset(&tv, 0, sizeof(struct timeval));
    memset(&tm, 0, sizeof(struct tm));

    gettimofday(&tv, NULL);
    // 获取当前时间，返回的是自1970-01-01 00：00:00到现在经历的秒数【第二个参数是时区，一般不关心】

    sec = tv.tv_sec;                // 秒
    localtime_r(&sec, &tm);         // 把参数1的time_t转换为本地时间，保存到参数2中去，带_r的是线程安全版本
    tm.tm_mon++;                    // 月份要调整一下才正常
    tm.tm_year += 1900;             // 年份也要调整一下才正常

    u_char strcurrtime[40]={0};     // 先组合出一个当前时间字符串，格式形如： 2019/01/08 12:32:23

    ngx_slprintf(strcurrtime,
                (u_char *)-1,                       // 若是用一个u_char *接一个 (u_char *)-1,则得到的结果是 0xffffffff... 这个值足够大
                "%4d/%02d/%02d %02d:%02d:%02d",     // 格式是 年/月/日 时：分：秒
                tm.tm_year, tm.tm_mon,
                tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec
    );
    p = ngx_cpymem(errstr, strcurrtime,strlen((const char *)strcurrtime));
    // 日期增加进来，得到形如   2019/01/08 20:26:07
    p = ngx_slprintf(p, last, " [%s] ", err_levels[level]);
    // 日志级别加进来，得到形如： 2019/01/08 20:26:07 [crit] 
    p = ngx_slprintf(p, last, "%p: ", ngx_pid);
    // 支持%p格式，进程ID增加进来，得到形如：2019/01/08 20:50:15 [crit] 2037:

    va_start(args, fmt);                // 使得args指向其实参数
    p = ngx_vslprintf(p, last, fmt, args); 
    // 把fmt和args参数弄进去，组合出来这个字符串
    va_end(args);                       // 释放args
    
    if(err)     // 如果错误代码不是0，表示有错误发生
    {
        // 错误代码和错误信息也要心事出来
        p = ngx_log_errno(p, last, err);
    }
    // 如果位置不够，那换行也要硬插入到末尾，哪怕覆盖其他内容
    if(p>=(last -1))
    {
        p = (last -1) - 1;
    }
    *p++ = '\n';
    // 增加换行符

    // 这么写是为了图方便：随时可以把流程弄到while后面去
    ssize_t n;
    while (1)
    {
        if(level > ngx_log.log_level)
        {
            // 要打印的这个日志等级态落后，（等级数字太大，比配置文件中的数字大）
            // 这种日志就不打印了
            break;
        }

        // 磁盘是否满了判断
        // todolist

        // 写日志文件
        n = write(ngx_log.fd, errstr, p-errstr);
        // 文件写入成功后，如果中途
        if(n==-1)
        {
            // 写入失败
            if(errno == ENOSPC) // 写失败了，且原因是磁盘没空间了
            {
                // todo
            }
            else
            {
                // 这里有其他错误，考虑把这些错误显示到标准错误设备
                if(ngx_log.fd != STDERR_FILENO) // 当前是定位到文件的，则条件成立
                {
                    n = write(STDERR_FILENO, errstr, p - errstr);
                }
            }
        }

        break;

    }
    
    return;
    
}

// ---------------------------------------------------------------------------------------------------------------------
// 描述：日志初始化，就是把日志文件打开，这里涉及到释放问题，如何解决
void ngx_log_init()
{
    u_char *plogname = NULL;
    size_t nlen;

    // 从配置文件中读取日志相关的配置信息
    CConfig *p_config = CConfig::GetInstance();
    plogname = (u_char *)p_config->GetString("Log");
    if(plogname == NUll)
    {
        // 没读到，就需要提供一个缺省的路径文件名
        plogname = (u_char *)NGX_ERROR_LOG_PATH;    // "logs/error.log", logs目录需要提前建立出来

    }

    ngx_log.log_level = p_config->GetIntDefault("LogLevel", NGX_LOG_NOTICE);
    // 缺省的日志等级为 6 【注意】,如果读失败，就给缺省的日志等级
    // nlen = strlen((const char *)plogname);

    // 只写打开|追加到末尾|文件不存在则创建文件 【这3个参数指定文件访问权限】
    // mode = 0644:文件访问权限， 6:110， 4:100， 【用户：读写，   用户所在组：读，   其他：读】
    ngx_log.fd = open((const char *)plogname, O_WRONLY|O_APPEND|O_CREAT, 0644);
    if(ngx_log.fd==-1)  // 如果有错误，则直接定位到 标准错误上去
    {
        ngx_log_stderr(errno, "[alert] could not open error log file: open() \"%s\" failed", plogname);
        ngx_log.fd = STDERR_FILENO; // 直接定位到标准错误
    }
    
    return;
}