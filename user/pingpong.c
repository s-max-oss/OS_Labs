#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    int f2s[2]; // father -> son
    int s2f[2]; // son -> father
    if(pipe(f2s) < 0 || pipe(s2f) < 0){
        printf("pipe error\n");
        exit(-1);
    }

    int pid = fork();
    if(pid < 0){
        printf("fork error\n");
        exit(-1);
    }

    if(pid == 0){
        // 子进程
        close(f2s[1]);
        close(s2f[0]);

        uint64 father_pid;
        // 从管道读取父进程pid
        read(f2s[0], &father_pid, sizeof(father_pid));
        int child_pid = getpid();

        printf("%d: received ping from pid %d\n", child_pid, father_pid);

        // 子把自己pid发给父
        uint64 send_child_pid = child_pid;
        write(s2f[1], &send_child_pid, sizeof(send_child_pid));

        close(f2s[0]);
        close(s2f[1]);
        exit(0);
    } else {
        // 父进程
        close(f2s[0]);
        close(s2f[1]);

        int father_pid = getpid();
        // 父把自己pid写入管道传给子
        write(f2s[1], &father_pid, sizeof(father_pid));

        uint64 child_pid;
        read(s2f[0], &child_pid, sizeof(child_pid));
        printf("%d: received pong from pid %d\n", father_pid, child_pid);

        close(f2s[1]);
        close(s2f[0]);
        wait(0);
    }
    exit(0);
}
