#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_number(const char *text, unsigned maximum, unsigned *number)
{
    if (*text < '0' || *text > '9') {
        return false;
    }
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value < 1 || value > maximum) {
        return false;
    }
    *number = (unsigned)value;
    return true;
}

static int read_number(const char *prompt, unsigned maximum, unsigned *number)
{
    char line[80];
    if (fputs(prompt, stdout) == EOF || fflush(stdout) == EOF) {
        perror("write prompt");
        return -1;
    }
    if (!fgets(line, sizeof line, stdin)) {
        if (ferror(stdin) && !ipc_interrupted) {
            perror("read input");
        }
        return -1;
    }
    char *newline = strchr(line, '\n');
    if (newline) {
        *newline = '\0';
    } else if (!feof(stdin)) {
        int ch;
        do {
            ch = getchar();
        } while (ch != '\n' && ch != EOF);
        return 0;
    }
    return parse_number(line, maximum, number) ? 1 : 0;
}

static int run_demo(unsigned index)
{
    const IPCTransport *transport = ipc_transports[index - 1];
    unsigned count = index == 1 ? 1 : 3;
    IPCResult result;
    printf("\n--- %s ---\n", transport->name);
    if (ipc_run(transport, count, true, &result) == -1) {
        fprintf(stderr, "%s demo failed.\n", transport->name);
        return 1;
    }
    printf("Delivery successful. Verified %u/%u in %.3f ms.\n",
           result.verified, count, result.milliseconds);
    return 0;
}

static int run_menu(void)
{
    while (!ipc_interrupted) {
        puts("\n=========================================\n"
             "       IPC COMMUNICATION SYSTEM\n"
             "=========================================\n"
             "1. Anonymous Pipes\n"
             "2. Named Pipes (FIFOs)\n"
             "3. Message Queues\n"
             "4. Shared Memory + Semaphores\n"
             "5. TCP Socket Communication\n"
             "6. Run Performance Benchmark\n"
             "7. View Comparison Report\n"
             "8. Exit");

        unsigned choice;
        int input = read_number("Enter your choice: ", 8, &choice);
        if (input == -1 || (input == 1 && choice == 8)) {
            break;
        }
        if (input == 0) {
            puts("Enter a number from 1 to 8.");
        } else if (choice <= IPC_TRANSPORT_COUNT) {
            run_demo(choice);
        } else if (choice == 6) {
            unsigned count;
            unsigned trials;
            int entered = read_number("Messages per trial (1-1000000): ",
                                      IPC_MAX_MESSAGES, &count);
            if (entered == 1) {
                entered = read_number("Trials per mechanism (1-20): ",
                                      IPC_MAX_TRIALS, &trials);
            }
            if (entered == -1) {
                break;
            }
            if (entered == 0) {
                puts("Enter a whole number in the indicated range.");
            } else {
                benchmark_run(count, trials);
            }
        } else {
            benchmark_report();
        }
    }
    return ferror(stdin) && !ipc_interrupted ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    if (ipc_install_signals() == -1) {
        perror("configure signals");
        return EXIT_FAILURE;
    }
    if (argc == 1) {
        return run_menu();
    }
    unsigned count;
    unsigned trials;
    if (argc == 3 && strcmp(argv[1], "demo") == 0 &&
        parse_number(argv[2], IPC_TRANSPORT_COUNT, &count)) {
        return run_demo(count);
    }
    if (argc == 4 && strcmp(argv[1], "benchmark") == 0 &&
        parse_number(argv[2], IPC_MAX_MESSAGES, &count) &&
        parse_number(argv[3], IPC_MAX_TRIALS, &trials)) {
        return benchmark_run(count, trials);
    }
    if (argc == 2 && strcmp(argv[1], "report") == 0) {
        return benchmark_report();
    }
    if (argc == 3 && strcmp(argv[1], "fifo-receive") == 0) {
        return fifo_receive(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "fifo-send") == 0) {
        return fifo_send(argv[2], argv[3]);
    }

    fprintf(stderr, "Usage: %s [demo 1-5 | benchmark MESSAGES TRIALS | report\n"
                    "       | fifo-receive PATH | fifo-send PATH TEXT]\n", argv[0]);
    return EXIT_FAILURE;
}
