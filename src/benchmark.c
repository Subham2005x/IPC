#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#define RESULTS_PATH "results/benchmark_results.csv"
#define CSV_HEADER "mechanism,messages_per_trial,trials,average_ms,messages_per_second,verified_total\n"

typedef struct {
    unsigned count;
    unsigned trials;
    unsigned verified;
    double milliseconds;
    double throughput;
} BenchmarkRow;

static void print_comparison(void)
{
    puts("\nMechanism          Ease         Synchronization / trade-offs");
    puts("Anonymous pipe     Simple       Blocking stream; inherited ends, one-way here");
    puts("Named pipe (FIFO)  Moderate     Blocking stream; pathname for independent processes");
    puts("SysV message queue Moderate     Kernel blocks on full/empty; typed records, queue limits");
    puts("Shared memory      More setup   Semaphores protect the slot; no kernel payload copy");
    puts("TCP socket         More setup   Blocking byte stream; network capable, framing required");
}

static void print_results(const BenchmarkRow rows[IPC_TRANSPORT_COUNT])
{
    printf("\n%-28s %10s %12s %16s %18s\n", "IPC Mechanism", "Messages",
           "Avg. ms", "Messages/sec", "Verified / total");
    for (size_t i = 0; i < IPC_TRANSPORT_COUNT; ++i) {
        const BenchmarkRow *row = &rows[i];
        printf("%-28s %10u %12.3f %16.0f %8u / %-8u\n",
               ipc_transports[i]->name, row->count, row->milliseconds,
               row->throughput, row->verified, row->count * row->trials);
    }

    bool ranked[IPC_TRANSPORT_COUNT] = {false};
    puts("\nRanking by measured throughput");
    for (size_t rank = 1; rank <= IPC_TRANSPORT_COUNT; ++rank) {
        size_t best = IPC_TRANSPORT_COUNT;
        for (size_t i = 0; i < IPC_TRANSPORT_COUNT; ++i) {
            if (!ranked[i] && (best == IPC_TRANSPORT_COUNT ||
                               rows[i].throughput > rows[best].throughput)) {
                best = i;
            }
        }
        ranked[best] = true;
        printf("%zu. %s\n", rank, ipc_transports[best]->name);
    }
}

static int save_results(const BenchmarkRow rows[IPC_TRANSPORT_COUNT])
{
    if (mkdir("results", 0755) == -1 && errno != EEXIST) {
        perror("create results directory");
        return 1;
    }
    char temporary[] = "results/.benchmark-XXXXXX";
    int fd = mkstemp(temporary);
    if (fd == -1) {
        perror("create temporary CSV");
        return 1;
    }
    FILE *file = fdopen(fd, "w");
    int status = 0;
    if (!file) {
        perror("open CSV stream");
        if (close(fd) == -1) {
            perror("close temporary CSV");
        }
        status = 1;
    } else {
        bool failed = fputs(CSV_HEADER, file) == EOF;
        for (size_t i = 0; i < IPC_TRANSPORT_COUNT && !failed; ++i) {
            const BenchmarkRow *row = &rows[i];
            failed = fprintf(file, "%s,%u,%u,%.9f,%.9f,%u\n", ipc_transports[i]->name,
                             row->count, row->trials, row->milliseconds,
                             row->throughput, row->verified) < 0;
        }
        if (failed) {
            perror("write CSV");
            status = 1;
        }
        if (fclose(file) == EOF) {
            perror("close CSV");
            status = 1;
        }
    }
    if (status == 0 && rename(temporary, RESULTS_PATH) == 0) {
        printf("\nSaved %s\n", RESULTS_PATH);
        return 0;
    }
    if (status == 0) {
        perror("replace CSV");
    }
    if (unlink(temporary) == -1) {
        perror("remove temporary CSV");
    }
    return 1;
}

int benchmark_run(unsigned count, unsigned trials)
{
    BenchmarkRow rows[IPC_TRANSPORT_COUNT];
    printf("\nBenchmarking %u messages per trial, %u trials per mechanism.\n",
           count, trials);
    for (size_t i = 0; i < IPC_TRANSPORT_COUNT; ++i) {
        const IPCTransport *transport = ipc_transports[i];
        BenchmarkRow row = { .count = count, .trials = trials };
        double total_ms = 0.0;
        for (unsigned trial = 1; trial <= trials; ++trial) {
            IPCResult result;
            if (ipc_run(transport, count, false, &result) == -1 ||
                result.milliseconds <= 0.0) {
                fprintf(stderr, "%s trial %u failed.\n", transport->name, trial);
                return 1;
            }
            total_ms += result.milliseconds;
            row.verified += result.verified;
        }
        row.milliseconds = total_ms / trials;
        row.throughput = row.verified * 1000.0 / total_ms;
        rows[i] = row;
        printf("Completed %s (%u/%u verified).\n",
               transport->name, row.verified, count * trials);
    }
    print_results(rows);
    return save_results(rows);
}

int benchmark_report(void)
{
    print_comparison();
    FILE *file = fopen(RESULTS_PATH, "r");
    if (!file) {
        if (errno == ENOENT) {
            puts("\nRun the benchmark to produce measured results.");
            return 0;
        }
        perror("open benchmark CSV");
        return 1;
    }

    char line[256];
    BenchmarkRow rows[IPC_TRANSPORT_COUNT];
    bool valid = fgets(line, sizeof line, file) && strcmp(line, CSV_HEADER) == 0;
    for (size_t i = 0; i < IPC_TRANSPORT_COUNT && valid; ++i) {
        BenchmarkRow *row = &rows[i];
        char name[64];
        valid = fgets(line, sizeof line, file) &&
                sscanf(line, "%63[^,],%u,%u,%lf,%lf,%u", name, &row->count,
                       &row->trials, &row->milliseconds, &row->throughput,
                       &row->verified) == 6 &&
                strcmp(name, ipc_transports[i]->name) == 0 &&
                row->count > 0 && row->count <= IPC_MAX_MESSAGES &&
                row->trials > 0 && row->trials <= IPC_MAX_TRIALS &&
                row->verified == row->count * row->trials &&
                row->milliseconds > 0.0 && row->throughput > 0.0;
    }
    if (ferror(file)) {
        perror("read benchmark CSV");
        valid = false;
    }
    if (fclose(file) == EOF) {
        perror("close benchmark CSV");
        return 1;
    }
    if (!valid) {
        fprintf(stderr, "Invalid benchmark CSV. Run the benchmark to replace it.\n");
        return 1;
    }
    print_results(rows);
    return 0;
}
