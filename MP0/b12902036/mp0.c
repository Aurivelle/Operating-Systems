#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/fs.h"

#define MAX_LEN 10
#define MAX_DEPTH 4
#define MAX_PATH 154

char *path_buffer;
char key;
int file_count = 0;
int dir_count = 0;

int count_key(char *str)
{
    int cnt = 0;
    for (; *str; str++)
    {
        if (*str == key)
        {
            cnt++;
        }
    }
    return cnt;
}

void traverse(int depth, int path_len, int current_occurrence)
{
    struct stat statbuf;
    struct dirent dirbuf;
    int fd;
    if ((fd = open(path_buffer, O_RDONLY)) < 0)
    {
        printf("%s [error opening dir]\n", path_buffer);
        return;
    }
    fstat(fd, &statbuf);

    if (depth == 0 && statbuf.type != T_DIR)
    {
        printf("%s [error opening dir]\n", path_buffer);
        close(fd);
        return;
    }

    printf("%s %d\n", path_buffer, current_occurrence);

    if (depth > 0)
    {
        if (statbuf.type == T_FILE)
        {
            file_count++;
            close(fd);
            return;
        }
        dir_count++;
    }

    path_buffer[path_len] = '/';
    path_buffer[path_len + 1] = '\0';

    while (read(fd, &dirbuf, sizeof(dirbuf)) == sizeof(dirbuf))
    {
        if (dirbuf.inum == 0 || strcmp(dirbuf.name, ".") == 0 || strcmp(dirbuf.name, "..") == 0)
        {
            continue;
        }

        strcpy(path_buffer + path_len + 1, dirbuf.name);
        int new_len = path_len + 1 + strlen(dirbuf.name);

        traverse(depth + 1, new_len, current_occurrence + count_key(dirbuf.name));
    }

    path_buffer[path_len] = '\0';
    close(fd);
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("Usage: mp0 <directory> <key>\n");
        exit(1);
    }
    char *directory = argv[1];
    key = argv[2][0];

    struct stat st;
    if (stat(directory, &st) < 0 || st.type != T_DIR)
    {
        printf("%s [error opening dir]\n", directory);
        printf("\n0 directories, 0 files\n");
        exit(1);
    }

    int fd[2];
    pipe(fd);

    if (fork() == 0)
    {
        close(fd[0]);

        path_buffer = (char *)malloc(MAX_PATH + 1);
        strcpy(path_buffer, directory);

        int init_occurrence = count_key(directory);
        traverse(0, strlen(path_buffer), init_occurrence);

        write(fd[1], &dir_count, sizeof(dir_count));
        write(fd[1], &file_count, sizeof(file_count));

        free(path_buffer);
        exit(0);
    }
    else
    {
        close(fd[1]);
        int ret_dir, ret_file;
        read(fd[0], &ret_dir, sizeof(ret_dir));
        read(fd[0], &ret_file, sizeof(ret_file));
        printf("\n%d directories, %d files\n", ret_dir, ret_file);
    }
    exit(0);
}