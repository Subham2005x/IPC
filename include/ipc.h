#ifndef IPC_H
#define IPC_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int message_id;
    pid_t sender_pid;
    char sender_name[32];
    char message[256];
} IPCMessage;

/* Return 1 on success, 0 on EOF before any byte, or -1 on error. */
int ipc_read_full(int fd, void *buffer, size_t length);

/* Return 0 on success or -1 on error. */
int ipc_write_full(int fd, const void *buffer, size_t length);

typedef struct {
    int read_fd;
    int write_fd;
    int id;
    void *shared;
    unsigned short port;
    char path[256];
} IPCChannel;

/* Setup runs before fork. Start and finish run in each process. Cleanup
 * releases local resources in both processes and owned resources in the parent.
 * Each callback returns 0 or -1 with errno set. Start and finish are optional. */
typedef struct {
    const char *name;
    int (*setup)(IPCChannel *channel);
    int (*start)(IPCChannel *channel, bool sender);
    int (*send)(IPCChannel *channel, const IPCMessage *message);
    int (*receive)(IPCChannel *channel, IPCMessage *message);
    int (*finish)(IPCChannel *channel, bool sender);
    int (*cleanup)(IPCChannel *channel, bool owner);
} IPCTransport;

typedef struct {
    unsigned verified;
    double milliseconds;
} IPCResult;

extern const IPCTransport pipe_transport;
extern const IPCTransport fifo_transport;
extern const IPCTransport queue_transport;
extern const IPCTransport shared_transport;
extern const IPCTransport tcp_transport;

enum {
    IPC_TRANSPORT_COUNT = 5,
    IPC_MAX_MESSAGES = 1000000,
    IPC_MAX_TRIALS = 20,
    IPC_QUEUE_MESSAGE_TYPE = 1
};
extern const IPCTransport *const ipc_transports[IPC_TRANSPORT_COUNT];
extern volatile sig_atomic_t ipc_interrupted;

int ipc_install_signals(void);
int ipc_close(int *fd);
int ipc_run(const IPCTransport *transport, unsigned count, bool verbose,
            IPCResult *result);
int fifo_send(const char *path, const char *text);
int fifo_receive(const char *path);
int benchmark_run(unsigned count, unsigned trials);
int benchmark_report(void);

#endif
