#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    int parent_to_child[2];  //读端和写端
    int child_to_parent[2];
    char buffer = 'A';
    // 创建两个管道
    if (pipe(parent_to_child) < 0 || pipe(child_to_parent) < 0) {
        fprintf(2, "创建管道失败！\n");
        exit(1);
    }
    int pid = fork();
    if (pid < 0) {
        fprintf(2, "创建子进程失败！\n");
        exit(1);
    }
    if (pid == 0) {  //此时是子进程
        close(parent_to_child[1]);  // 关闭写端
        close(child_to_parent[0]);  // 关闭读端

        // 读取父进程发送的数据
        read(parent_to_child[0], &buffer, 1);
        printf("%d: received ping\n", getpid());

        // 向父进程发送数据
        write(child_to_parent[1], &buffer, 1);
        close(parent_to_child[0]);
        close(child_to_parent[1]);
        exit(0);
    } 
    else {// 父进程
        close(parent_to_child[0]);  // 关闭读端
        close(child_to_parent[1]);  // 关闭写端
        write(parent_to_child[1], &buffer, 1);
        read(child_to_parent[0], &buffer, 1);
        printf("%d: received pong\n", getpid());
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        exit(0);
    }
}