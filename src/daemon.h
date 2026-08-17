/*
 * daemon.h / daemon.c
 * 进程守护化与信号处理
 */
#ifndef DAEMON_H
#define DAEMON_H

/*
 * 将当前进程转为守护进程
 * 标准 UNIX 双重 fork：脱离终端 + 新建会话 + I/O 重定向到 /dev/null
 * 返回: 0 成功，-1 失败
 */
int daemonize(void);

#endif /* DAEMON_H */
