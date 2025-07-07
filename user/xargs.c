#include "kernel/types.h"
#include "user/user.h"
#include "kernel/param.h"
#include <stddef.h>

#define MAXLINE 1024

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(2, "Usage: xargs command [args...]\n");
        exit(1);
    }
    // 复制命令行参数到base_argv
    char *base_argv[MAXARG];
    int i;
    for (i = 1; i < argc && i < MAXARG; i++) {
        base_argv[i-1] = argv[i];
    }
    int base_argc = i - 1;
    char line[MAXLINE];
    int len = 0;
    char c;
    // 从标准输入读取行
    while (read(0, &c, 1) > 0) {
        if (c == '\n' || len >= MAXLINE - 1) {
            line[len] = '\0';         // 终止字符串
            char *full_argv[MAXARG];  // 为当前行创建完整的参数数组
            for (int i = 0; i < base_argc; i++) {
                full_argv[i] = base_argv[i];
            }
            full_argv[base_argc] = line;      // 添加当前行作为额外参数
            full_argv[base_argc + 1] = NULL;  // 参数数组必须以NULL结尾
            int pid = fork();         // 创建子进程执行命令
            if (pid < 0) {
                fprintf(2, "进程创建失败！\n");
                exit(1);
            } else if (pid == 0) {
                exec(base_argv[0], full_argv);
                // 如果exec返回，说明执行失败
                fprintf(2, "exec执行命令失败！ %s\n", base_argv[0]);
                exit(1);
            } else {
                wait(0);
            }
            len = 0;  // 重置行长度
        } else {
            line[len++] = c;
        }
    }
    exit(0);
}