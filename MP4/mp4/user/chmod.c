#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user/user.h"
#include "kernel/fcntl.h"

#define MAXPATH 128

#ifndef M_READ
#define M_READ 1
#define M_WRITE 2
#endif

/* 只有一個輔助函式：安全字串複製 */
static void safestrcpy(char *dst, const char *src, int n)
{
    if (n <= 0)
        return;
    while (--n > 0 && *src)
        *dst++ = *src++;
    *dst = '\0';
}

/* 解析 symlink → out 為最終實體路徑；若失敗傳 -1 */
static int resolve(const char *in, char out[MAXPATH])
{
    char buf[MAXPATH], tmp[MAXPATH];
    safestrcpy(buf, in, MAXPATH);
    while (1)
    {
        int fd = open(buf, O_NOACCESS);
        if (fd < 0)
            return -1;
        struct stat st;
        if (fstat(fd, &st) < 0)
        {
            close(fd);
            return -1;
        }
        if (st.type != T_SYMLINK)
        {
            close(fd);
            break;
        }
        int n = read(fd, tmp, sizeof(tmp) - 1);
        close(fd);
        if (n <= 0)
            return -1;
        tmp[n] = '\0';
        safestrcpy(buf, tmp, MAXPATH);
    }
    safestrcpy(out, buf, MAXPATH);
    return 0;
}

int main(int argc, char *argv[])
{
    /* ───── 參數解析 ───── */
    int recursive = 0, op = 0, bits = 0;
    char *mode_str, *target;
    if (argc == 4 && !strcmp(argv[1], "-R"))
    {
        recursive = 1;
        mode_str = argv[2];
        target = argv[3];
    }
    else if (argc == 3)
    {
        mode_str = argv[1];
        target = argv[2];
    }
    else
    {
        fprintf(2, "Usage: chmod [-R] (+|-)(r|w|rw|wr) file_name|dir_name\n");
        exit(1);
    }

    /* (+|-)rw 解析 */
    if (!(mode_str[0] == '+' || mode_str[0] == '-'))
    {
        fprintf(2, "Usage: chmod [-R] (+|-)(r|w|rw|wr) file_name|dir_name\n");
        exit(1);
    }
    op = (mode_str[0] == '+') ? +1 : -1;
    for (int i = 1; mode_str[i]; i++)
    {
        if (mode_str[i] == 'r')
            bits |= M_READ;
        else if (mode_str[i] == 'w')
            bits |= M_WRITE;
        else
        {
            fprintf(2,
                    "Usage: chmod [-R] (+|-)(r|w|rw|wr) file_name|dir_name\n");
            exit(1);
        }
    }
    if (bits == 0)
    {
        fprintf(2, "Usage: chmod [-R] (+|-)(r|w|rw|wr) file_name|dir_name\n");
        exit(1);
    }

    /* 保存使用者原始輸入 (錯誤訊息要用) */
    char g_origin[MAXPATH];
    safestrcpy(g_origin, target, MAXPATH);

    /* ───── symlink 解析 ───── */
    char root[MAXPATH];
    if (resolve(target, root) < 0)
    {
        fprintf(2, "chmod: cannot chmod %s\n", target);
        exit(1);
    }

    /* ───── 非遞迴模式：直接 chmod 後結束 ───── */
    if (!recursive)
    {
        int fd = open(root, O_NOACCESS);
        if (fd < 0)
        {
            fprintf(2, "chmod: cannot open %s\n", root);
            exit(1);
        }
        struct stat st;
        if (fstat(fd, &st) < 0)
        {
            close(fd);
            fprintf(2, "chmod: cannot stat %s\n", root);
            exit(1);
        }
        close(fd);
        int newmode = (op > 0) ? (st.mode | bits) : (st.mode & ~bits);
        if (chmod(newmode, root) < 0)
            fprintf(2, "chmod: cannot chmod %s\n", root);
        exit(0);
    }

    /* ───── 遞迴前檢查 root 可讀性 (規格要求) ───── */
    {
        int fd = open(root, O_NOACCESS);
        if (fd < 0)
        {
            fprintf(2, "chmod: cannot chmod %s\n", target);
            exit(1);
        }
        struct stat st;
        if (fstat(fd, &st) < 0)
        {
            close(fd);
            fprintf(2, "chmod: cannot chmod %s\n", target);
            exit(1);
        }
        close(fd);
        if (!(st.mode & M_READ) && op < 0)
        {
            fprintf(2, "chmod: cannot chmod %s\n", target);
            exit(1);
        }
    }

    /* ───── 迭代 DFS (僅此一函式實作) ───── */
    /* 迭代用顯式 stack：宣告在全域，避免壓爆使用者執行緒的 8 KB stack */
    struct Frame
    {
        char path[MAXPATH];
        int need_tmp_r;
        int post;
    };
    static struct Frame stack[1024];
    int top = 0;
    safestrcpy(stack[0].path, root, MAXPATH);
    stack[0].need_tmp_r = 0;
    stack[0].post = 0;

    while (top >= 0)
    {
        struct Frame cur = stack[top--];

        /* ───── Post‑order：真正執行 chmod + 還原 tmp r ───── */
        if (cur.post)
        {
            int fd = open(cur.path, O_NOACCESS);
            if (fd < 0)
            {
                fprintf(2, "chmod: cannot open %s\n", cur.path);
            }
            else
            {
                struct stat st;
                if (fstat(fd, &st) < 0)
                {
                    close(fd);
                    fprintf(2, "chmod: cannot stat %s\n", cur.path);
                }
                else
                {
                    int newmode =
                        (op > 0) ? (st.mode | bits) : (st.mode & ~bits);
                    if (chmod(newmode, cur.path) < 0)
                        fprintf(2, "chmod: cannot chmod %s\n", cur.path);
                    close(fd);
                }
            }
            if (cur.need_tmp_r && op < 0 && (bits & M_READ))
            {
                int fd = open(cur.path, O_NOACCESS);
                if (fd >= 0)
                {
                    struct stat st;
                    if (fstat(fd, &st) == 0)
                    {
                        int nm = st.mode & ~M_READ;
                        if (chmod(nm, cur.path) < 0)
                            fprintf(2, "chmod: cannot restore r from %s\n",
                                    cur.path);
                    }
                    close(fd);
                }
            }
            continue;
        }

        /* ───── Pre‑order：檢查型別、暫時 +r、push 子項 ───── */
        int fd = open(cur.path, O_NOACCESS);
        if (fd < 0)
        {
            fprintf(2, "chmod: cannot chmod %s\n", g_origin);
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) < 0)
        {
            close(fd);
            fprintf(2, "chmod: cannot chmod %s\n", g_origin);
            continue;
        }
        close(fd);

        int is_dir = (st.type == T_DIR);
        if (is_dir && op < 0 && !(st.mode & M_READ))
        {
            fprintf(2, "chmod: cannot chmod %s\n", g_origin);
            continue;
        }

        int need_tmp_r = 0;
        if (is_dir && op > 0 && (bits & M_READ) && !(st.mode & M_READ))
        {
            int nm = st.mode | M_READ;
            if (chmod(nm, cur.path) < 0)
            {
                fprintf(2, "chmod: cannot chmod %s\n", g_origin);
                continue;
            }
            need_tmp_r = 1;
        }

        /* push postorder frame */
        ++top;
        safestrcpy(stack[top].path, cur.path, MAXPATH);
        stack[top].need_tmp_r = need_tmp_r;
        stack[top].post = 1;

        /* 探索子目錄 */
        if (is_dir)
        {
            int dfd = open(cur.path, O_RDONLY);
            if (dfd < 0)
            {
                fprintf(2, "chmod: cannot open directory %s\n", cur.path);
                continue;
            }
            struct dirent de;
            char buf[512];
            while (read(dfd, &de, sizeof(de)) == sizeof(de))
            {
                if (de.inum == 0 || !strcmp(de.name, ".") ||
                    !strcmp(de.name, ".."))
                    continue;
                safestrcpy(buf, cur.path, sizeof(buf));
                int len = strlen(buf);
                buf[len++] = '/';
                int nlen = strlen(de.name);
                if (nlen > DIRSIZ)
                    nlen = DIRSIZ;
                memmove(buf + len, de.name, nlen);
                buf[len + nlen] = '\0';
                char real[MAXPATH];
                if (resolve(buf, real) < 0)
                {
                    fprintf(2, "chmod: cannot resolve %s\n", buf);
                    continue;
                }
                ++top;
                safestrcpy(stack[top].path, real, MAXPATH);
                stack[top].need_tmp_r = 0;
                stack[top].post = 0;
            }
            close(dfd);
        }
    }
    exit(0);
}
