#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void main()
{
    //设置HTML语言
    printf("Content-type:text/html\n\n");

    char szPost[256] = {0};

    //获取输入
    gets(szPost);

    //打印输入的内容
    printf("<pre>%s</pre>", szPost);
    //BBB=tasklist&AAA=%C7%EB%BD%F8

    char *p = szPost + 4;
    char *p1 = strchr(szPost, '&');
    *p1 = '\0';

    char cmd[256] = {0};

    //字符串映射
    sprintf(cmd, "%s > 1.txt", p);

    system(cmd);

    //以读的方式打开1.txt
    FILE* fp = fopen("1.txt", "r");

    //循环成立的条件是没有读到文件结尾
    while(!feof(fp))
    {
        //每次从文件中读取1个字符
        char ch = fgetc(fp);

        //当读取到\n时
        if('\n' == ch)
        {
            //打印换行
            printf("<br>");
        }
        else
        {
            //打印字符
            putchar(ch);
        }
    }
}
