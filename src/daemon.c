#include "daemon.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) exit(0); /* 父进程退出 */

    if (setsid() < 0) return -1;

    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) exit(0); /* 再次脱离终端 */

    if (chdir("/") < 0) return -1;

    /* 关闭标准文件描述符 */
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }

    return 0;
}
