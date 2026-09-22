#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char *fmtname(char *path) {
    static char buf[DIRSIZ + 1];
    char *p;
    for (p = path + strlen(path); p >= path && *p != '/'; p--);
    p++;
    if (strlen(p) >= DIRSIZ) return p;
    memmove(buf, p, strlen(p));
    memset(buf + strlen(p), ' ', DIRSIZ - strlen(p));
    return buf;
}

void getname(char *path, char *namebuf) {
    char *p;
    for (p = path + strlen(path); p >= path && *p != '/'; p--);
    p++;
    strcpy(namebuf, p);
}

void findfiles(char *path, char *filename) {
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;
    if ((fd = open(path, 0)) < 0) {
        printf("find: cannot open %s\n", path);
        return;
    }

    if (fstat(fd, &st) < 0) {
        printf("find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    switch (st.type) {
        case T_FILE:
        {
            char namebuf[DIRSIZ+1];
            getname(path, namebuf);
            if(strcmp(namebuf, filename) == 0){
                printf("%s\n", path);
            }
            break;
        }
        case T_DIR:
        {
            char namebuf[DIRSIZ+1];
            getname(path, namebuf);
            if(strcmp(namebuf, filename) == 0){
                printf("%s\n", path);
            }
            if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
                printf("find: path too long\n");
                break;
            }
            strcpy(buf, path);
            p = buf + strlen(buf);
            *p++ = '/';
            while (read(fd, &de, sizeof(de)) == sizeof(de)) {
                if (de.inum == 0) continue;
                // =========新增：跳过 . 和 .. =========
                if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) continue;
                memmove(p, de.name, DIRSIZ);
                p[DIRSIZ] = 0;
                struct stat child_st;
                if (stat(buf, &child_st) < 0) {
                    printf("find: cannot stat %s\n", buf);
                    continue;
                }
                findfiles(buf, filename);
            }
            break;
        }
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Need a file path and file name!\n");
        exit(-1);
    }
    findfiles(argv[1], argv[2]);
    exit(0);
}
