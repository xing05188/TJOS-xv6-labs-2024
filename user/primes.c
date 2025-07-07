#include "kernel/types.h"
#include "user/user.h"

void primes(int fd) __attribute__((noreturn));

int main(int argc, char *argv[]) {
    int p[2];  //只需一个管道
    pipe(p);

    int pid = fork();
    if (pid < 0) {
        fprintf(2, "创建进程失败！\n");
        exit(1);
    }
    if (pid == 0) {   // 子进程 - 第一个筛选器
        close(p[1]);  // 关闭写端
        primes(p[0]);
    } else {          // 父进程 - 生成2-280的数字
        close(p[0]);  // 关闭读端
        for (int i = 2; i <= 280; i++) {
            write(p[1], &i, sizeof(int));
        }
        close(p[1]);  // 写完所有数据后关闭写端
        wait(0);      // 等待子进程结束
        exit(0);
    }
}

void primes(int fd) {
    int prime;
    if (read(fd, &prime, sizeof(int)) == 0) { //没有数据了
        close(fd);
        exit(0);
    }
    printf("prime %d\n", prime);

    int p[2];
    pipe(p);
    int pid = fork();
    if (pid < 0) {
        fprintf(2, "创建进程失败！\n");
        exit(1);
    }
    if (pid == 0) {
        close(p[1]);  
        close(fd);  
        primes(p[0]);
    } else {
        close(p[0]);  
        int i;
        while (read(fd, &i, sizeof(int)) > 0) {
            if (i % prime != 0) {
                write(p[1], &i, sizeof(int));
            }
        }
        close(fd);    // 关闭当前读端
        close(p[1]);  // 关闭写端
        wait(0);      // 等待子进程结束
        exit(0);
    }
}