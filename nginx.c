#include <stdio.h>
#include <unistd.h>

// #include <signal.h>

int main()
{

   printf("hello Word \n");

    

   for(;;)
   {
      sleep(1); // 休息1秒
      printf("sleep 1s \n");
   }

   printf("程序退出！\n");

   return 0;
}
