#include "ipc.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const IPCTransport *const ipc_transports[IPC_TRANSPORT_COUNT] = {
    &pipe_transport, &fifo_transport, &queue_transport, &shared_transport,
    &tcp_transport
};

static int set_deadline(int seconds)
{
    struct itimerval timer = {
        .it_value.tv_sec = seconds,
        .it_interval.tv_sec = seconds ? 1 : 0
    };
    return setitimer(ITIMER_REAL, &timer, NULL);
}

static int receiver_deadline(unsigned count)
{
    /* Allow larger semaphore benchmarks more time on virtualized hosts. */
    return 20 + (int)(count / 10000);
}

static void close_checked(int *fd, int *status)
{
    if (ipc_close(fd) == -1) {
        perror("close status pipe");
        *status = -1;
    }
}

static int cleanup(const IPCTransport *transport, IPCChannel *channel, bool owner)
{
    if (transport->cleanup(channel, owner) == -1) {
        perror("release transport resources");
        return -1;
    }
    return 0;
}

static IPCMessage base_message(bool verbose)
{
    IPCMessage message = { .sender_pid = getpid(), .sender_name = "Parent" };
    if (verbose) {
        strcpy(message.message, "Hello from the parent process!");
    } else {
        for (size_t i = 0; i < sizeof message.message - 1; ++i) {
            message.message[i] = (char)('A' + i % 26);
        }
    }
    return message;
}

static bool matches(const IPCMessage *message, const IPCMessage *expected)
{
    return message->message_id == expected->message_id &&
           message->sender_pid == expected->sender_pid &&
           memcmp(message->sender_name, expected->sender_name,
                  sizeof message->sender_name) == 0 &&
           memcmp(message->message, expected->message, sizeof message->message) == 0;
}

static int check_frame(int fd, void *data, size_t length)
{
    int status = ipc_read_full(fd, data, length);
    if (status == 1) {
        return 0;
    }
    if (status == 0) {
        errno = EPROTO;
    }
    return -1;
}

static int receive_records(const IPCTransport *transport, IPCChannel *channel,
                           IPCMessage expected, unsigned count, bool verbose,
                           int status_fd)
{
    const unsigned char ready = 1;
    if (set_deadline(receiver_deadline(count)) == -1 ||
        (transport->start && transport->start(channel, false) == -1) ||
        ipc_write_full(status_fd, &ready, sizeof ready) == -1) {
        perror("prepare receiver");
        return -1;
    }

    unsigned verified = 0;
    for (unsigned i = 1; i <= count; ++i) {
        IPCMessage message;
        if (transport->receive(channel, &message) == -1) {
            perror("receive record");
            return -1;
        }
        expected.message_id = (int)i;
        if (matches(&message, &expected)) {
            ++verified;
            if (verbose) {
                printf("Receiver PID %jd verified #%d from %s (PID %jd): %s\n",
                       (intmax_t)getpid(), message.message_id, message.sender_name,
                       (intmax_t)message.sender_pid, message.message);
            }
        }
    }
    if (transport->finish && transport->finish(channel, false) == -1) {
        perror("finish receiving");
        return -1;
    }
    if (verbose && fflush(stdout) == EOF) {
        perror("print receiver output");
        return -1;
    }
    if (ipc_write_full(status_fd, &verified, sizeof verified) == -1) {
        perror("send verification count");
        return -1;
    }
    return 0;
}

static int send_records(const IPCTransport *transport, IPCChannel *channel,
                        IPCMessage message, unsigned count, bool verbose,
                        pid_t receiver, int status_fd, IPCResult *result)
{
    unsigned char ready;
    if ((transport->start && transport->start(channel, true) == -1) ||
        check_frame(status_fd, &ready, sizeof ready) == -1) {
        perror("prepare sender");
        return -1;
    }
    if (verbose && channel->port) {
        printf("TCP client connected to 127.0.0.1:%u\n", (unsigned)channel->port);
    }
    if (verbose && transport == &queue_transport) {
        printf("System V message type: %d (all records)\n", IPC_QUEUE_MESSAGE_TYPE);
    }

    struct timespec begin;
    struct timespec end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin) == -1) {
        perror("start clock");
        return -1;
    }
    for (unsigned i = 1; i <= count; ++i) {
        message.message_id = (int)i;
        if (verbose) {
            if (printf("Sender PID %jd -> receiver PID %jd, #%d: %s\n",
                       (intmax_t)message.sender_pid, (intmax_t)receiver,
                       message.message_id, message.message) < 0 || fflush(stdout) == EOF) {
                perror("print sender output");
                return -1;
            }
        }
        if (transport->send(channel, &message) == -1) {
            perror("send record");
            return -1;
        }
    }
    if (transport->finish && transport->finish(channel, true) == -1) {
        perror("finish sending");
        return -1;
    }
    if (check_frame(status_fd, &result->verified, sizeof result->verified) == -1) {
        perror("read verification count");
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) == -1) {
        perror("stop clock");
        return -1;
    }
    result->milliseconds = (end.tv_sec - begin.tv_sec) * 1000.0 +
                           (end.tv_nsec - begin.tv_nsec) / 1000000.0;
    if (result->verified != count) {
        fprintf(stderr, "Verification failed (%u of %u records matched).\n",
                result->verified, count);
        return -1;
    }
    if (verbose && channel->port) {
        puts("TCP receiver confirmed a clean disconnect.");
    }
    return 0;
}

static int reap_receiver(pid_t receiver, bool cancel)
{
    if (cancel && kill(receiver, SIGKILL) == -1 && errno != ESRCH) {
        perror("stop receiver");
    }

    int status;
    pid_t pid;
    do {
        pid = waitpid(receiver, &status, 0);
    } while (pid == -1 && errno == EINTR);
    if (pid == -1) {
        perror("waitpid");
        return -1;
    }
    if (!cancel && (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)) {
        fputs("Receiver process failed.\n", stderr);
        return -1;
    }
    return 0;
}

int ipc_run(const IPCTransport *transport, unsigned count, bool verbose,
            IPCResult *result)
{
    IPCChannel channel = { .read_fd = -1, .write_fd = -1, .id = -1 };
    IPCMessage message = base_message(verbose);
    int report[2] = {-1, -1};
    int status = -1;
    *result = (IPCResult){0};

    if (ipc_interrupted) {
        return -1;
    }
    if (transport->setup(&channel) == -1) {
        perror("set up transport");
    } else if (pipe(report) == -1) {
        perror("create verification pipe");
    } else if (fflush(stdout) == EOF) {
        perror("flush output before fork");
    } else {
        pid_t receiver = fork();
        if (receiver == -1) {
            perror("fork receiver");
        } else if (receiver == 0) {
            int child_status = 0;
            close_checked(&report[0], &child_status);
            if (child_status == 0) {
                child_status = receive_records(transport, &channel, message, count,
                                               verbose, report[1]);
            }
            close_checked(&report[1], &child_status);
            if (cleanup(transport, &channel, false) == -1) {
                child_status = -1;
            }
            _exit(child_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
        } else {
            status = 0;
            close_checked(&report[1], &status);
            if (set_deadline(receiver_deadline(count) + 5) == -1) {
                perror("set sender deadline");
                status = -1;
            }
            if (status == 0) {
                status = send_records(transport, &channel, message, count, verbose,
                                      receiver, report[0], result);
            }
            if (reap_receiver(receiver, status == -1) == -1) {
                status = -1;
            }
            if (set_deadline(0) == -1) {
                perror("clear sender deadline");
                status = -1;
            }
        }
    }

    close_checked(&report[0], &status);
    close_checked(&report[1], &status);
    if (cleanup(transport, &channel, true) == -1) {
        status = -1;
    }
    if (ipc_interrupted == SIGALRM) {
        fputs("Transfer exceeded its deadline.\n", stderr);
        ipc_interrupted = 0;
        status = -1;
    }
    return status;
}
