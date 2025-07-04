// user/ls.c  ── “[1] style”
// ======================================================================
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define MAXPATH 128

// ───── util ────────────────────────────────────────────────────────────
static void safestrcpy(char *dst, const char *src, int n)
{
    if (n <= 0)
        return;
    while (--n > 0 && *src)
        *dst++ = *src++;
    *dst = '\0';
}

// blank-padded name (同 [1] 的 fmtname)
static char *fmtname(char *path)
{
    static char buf[DIRSIZ + 1];
    char *p;

    for (p = path + strlen(path); p >= path && *p != '/'; p--)
        ;
    p++; // now p 指到最後一節

    if (strlen(p) >= DIRSIZ)
        return p;
    memmove(buf, p, strlen(p));
    memset(buf + strlen(p), ' ', DIRSIZ - strlen(p));
    return buf;
}

// ───── core ls ─────────────────────────────────────────────────────────
static void ls(char *path)
{
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;

    // ① 先用 stat() 取 metadata，且要求目標本身有 r 權限
    if (stat(path, &st) < 0 || !(st.mode & M_READ))
    {
        fprintf(2, "ls: cannot open %s\n", path);
        return;
    }

    // ② 使用 O_RDONLY 開啟；對檔案/目錄/連結皆可
    fd = open(path, 0); // 0 = O_RDONLY
    if (fd < 0)
    {
        fprintf(2, "ls: cannot open %s\n", path);
        return;
    }

    switch (st.type)
    {
    // ---------- 普通檔案 ----------
    case T_FILE:
    {
        char perm[3];
        perm[0] = (st.mode & M_READ) ? 'r' : '-';
        perm[1] = (st.mode & M_WRITE) ? 'w' : '-';
        perm[2] = '\0';
        printf("%s %d %d %d %s\n", fmtname(path), st.type, st.ino, st.size,
               perm);
        break;
    }

    // ---------- 目錄 ----------
    case T_DIR:
    {
        if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf))
        {
            printf("ls: path too long\n");
            break;
        }
        strcpy(buf, path);
        p = buf + strlen(buf);
        *p++ = '/';

        while (read(fd, &de, sizeof(de)) == sizeof(de))
        {
            if (de.inum == 0)
                continue;
            memmove(p, de.name, DIRSIZ);
            p[DIRSIZ] = 0;

            struct stat est;
            if (stat(buf, &est) < 0)
            {
                printf("ls: cannot stat %s\n", buf);
                continue;
            }

            char perm[3];
            perm[0] = (est.mode & M_READ) ? 'r' : '-';
            perm[1] = (est.mode & M_WRITE) ? 'w' : '-';
            perm[2] = '\0';

            printf("%s %d %d %d %s\n", fmtname(buf), est.type, est.ino,
                   est.size, perm);
        }
        break;
    }

    // ---------- 符號連結 ----------
    case T_SYMLINK:
    {
        char target[MAXPATH];
        safestrcpy(target, path, sizeof(target));

        struct stat tst;

        // 解析連結鍊直到不是 symlink 為止
        while (1)
        {
            int lfd = open(target, O_NOACCESS); // 不跟隨
            if (lfd < 0)
            {
                fprintf(2, "ls: cannot open %s\n", target);
                break;
            }
            if (fstat(lfd, &tst) < 0)
            {
                fprintf(2, "ls: cannot stat %s\n", target);
                close(lfd);
                break;
            }
            if (tst.type != T_SYMLINK)
            {
                close(lfd);
                break; // 得到最終 target metadata
            }
            int n = read(lfd, target, MAXPATH - 1);
            close(lfd);
            if (n < 0)
            {
                fprintf(2, "ls: cannot read %s\n", target);
                break;
            }
            target[n] = '\0';
        }

        if (tst.type == T_DIR)
        {
            // link 指向目錄 → 顯示目錄內容
            ls(target);
        }
        else
        {
            // link 指向檔案或無效 → 列印 link 本身資訊
            char perm[3] = "rw"; // symlink 本身固定 rw
            printf("%s %d %d %d %s\n", fmtname(path), T_SYMLINK, st.ino,
                   st.size, perm);
        }
        break;
    }
    } // end switch

    close(fd);
}

// ───── main ────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        ls(".");
        exit(0);
    }
    for (int i = 1; i < argc; i++)
        ls(argv[i]);
    exit(0);
}
