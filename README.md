# IPC Communication & Performance Testing System

A C11 application for studying inter-process communication (IPC) on Ubuntu Linux. IPC lets separate operating-system processes exchange data or coordinate access to a resource. This project demonstrates five Linux IPC mechanisms and measures the throughput of a verified one-way transfer for each.

## Run on Windows with Docker Desktop

Start Docker Desktop and wait for its Linux engine. Open PowerShell in the cloned project directory. The first build downloads the Ubuntu base image and installs the build tools in the image.

```powershell
docker build -t ipc-communication-system .
$results = (Resolve-Path -LiteralPath '.\results').Path
docker create --name ipc-system --mount "type=bind,source=$results,target=/app/results" --entrypoint /usr/bin/sleep ipc-communication-system infinity
```

Create the container once. To open the menu in that same container, run these commands from any PowerShell folder:

```powershell
docker start ipc-system
docker exec -it ipc-system ./ipc_system
```

Option 8 closes the menu. Use `docker stop ipc-system` to stop the container between sessions. The bind mount stores the generated CSV in the project’s `results` directory. After editing the source, build a new image and create a new container from it under a different name to test those changes. The existing container continues to use the image it was created from.

## Build and run directly on Ubuntu

From the project directory, install GCC, GNU Make, and the C development headers if needed, then run:

```bash
sudo apt update
sudo apt install build-essential
make
./ipc_system
```

The Makefile uses C11, `-Wall -Wextra -Wpedantic`, and `-pthread` for the POSIX semaphore API. Use `make clean` to remove generated object files and the executable.

## Use the menu

| Choice | What to do and what to expect |
| --- | --- |
| 1. Anonymous Pipes | Watch the parent send one record to the child through an inherited pipe. |
| 2. Named Pipes (FIFOs) | Watch the processes open a temporary FIFO by pathname and transfer three records. |
| 3. Message Queues | Look for System V message type 1 and three verified records. |
| 4. Shared Memory + Semaphores | Watch three records pass through a one-slot shared-memory buffer. |
| 5. TCP Socket Communication | Look for a connection to an ephemeral localhost port, verified records, and a clean disconnect. |
| 6. Run Performance Benchmark | Enter the message count per trial and the trial count. Start with `10000` and `3`. The maximum inputs are 1,000,000 messages and 20 trials. |
| 7. View Comparison Report | Read the implementation trade-offs and the measurements from the latest saved CSV. |
| 8. Exit | Leave the application. |

Press Enter after each input. Options 1–5 run automatically and return to the menu. Each prints distinct sender and receiver PIDs and a final verification count. For example, the anonymous pipe demo prints:

```text
--- Anonymous pipe ---
Sender PID 1 -> receiver PID 7, #1: Hello from the parent process!
Receiver PID 7 verified #1 from Parent (PID 1): Hello from the parent process!
Delivery successful. Verified 1/1 in 0.109 ms.
```

PIDs and timing vary. For direct invocation, use `./ipc_system demo 1`, `./ipc_system benchmark 100000 3`, or `./ipc_system report` on Ubuntu. In the running Docker container, for example, use `docker exec ipc-system ./ipc_system report`.

### Run the FIFO with independent processes

With the Docker container running, start the receiver in PowerShell:

```powershell
docker exec -it ipc-system ./ipc_system fifo-receive /tmp/ipc-example.fifo
```

After it prints that it is waiting, open a **second PowerShell window** and run:

```powershell
docker exec ipc-system ./ipc_system fifo-send /tmp/ipc-example.fifo 'Hello from the second terminal'
```

On Ubuntu, use two terminals in the project directory and run the same `fifo-receive` and `fifo-send` subcommands directly via `./ipc_system`. The receiver creates the FIFO, waits up to 30 seconds for one message, checks its framing and text checksum, prints the message, and removes the FIFO. The sender accepts text up to 255 bytes.

## Architecture

| File | Responsibility |
| --- | --- |
| `include/ipc.h` | Defines the common message, transport callbacks, and transfer results. |
| `src/main.c` | Parses menu input and direct commands. |
| `src/common.c` | Handles signals and complete file-descriptor reads and writes. |
| `src/session.c` | Starts the sender and receiver, verifies records, times transfers, and reaps the child. |
| `src/pipe_demo.c`, `src/fifo_demo.c` | Implement unnamed and named byte streams. |
| `src/message_queue.c`, `src/shared_memory.c` | Implement the two System V mechanisms. |
| `src/socket_demo.c` | Implements the localhost TCP client and server. |
| `src/benchmark.c` | Runs repeated trials, ranks throughput, writes the CSV, and loads the report. |
| `results/benchmark_results.csv` | Measurements from the latest complete benchmark run. |
| `screenshots/` | Place to keep terminal captures for the assignment. |

The menu and benchmark use the parent as sender and a child as receiver. The receiver validates the sequence number, sender PID, name, and all bytes of the payload before counting a record as verified. A separate control pipe carries readiness and the final verification count for every mechanism. The parent checks the receiver's exit status with `waitpid()` and reaps it before releasing the transport's resources. A transfer that stalls is cancelled and reported as a failure.

## How each mechanism works

| Mechanism | System calls and synchronization | Advantages | Limitations |
| --- | --- | --- | --- |
| Anonymous pipe | `pipe()` creates a read/write descriptor pair. After `fork()`, each process closes the end it does not use. Blocking `read()` and `write()` provide backpressure. | Small setup, useful for parent–child transfers. | Endpoints must already have the descriptors. A pipe carries bytes in one direction. |
| Named pipe (FIFO) | `mkfifo()` creates a filesystem entry. Processes use `open()` with the same path and transfer through `read()` and `write()`. The owner calls `unlink()`. | Independent processes can rendezvous by path. | Requires path coordination, suitable permissions, cleanup, and stream framing. |
| System V queue | `msgget()` creates a private queue. `msgsnd()` sends a record with a positive `long` type. `msgrcv()` selects type 1. The length passed to these calls excludes the type. `msgctl(IPC_RMID)` removes the queue. | Kernel preserves message boundaries and can select by type. | Requires System V resource management and has kernel queue limits. |
| Shared memory | `shmget()` allocates a segment, `shmat()` attaches it, and `fork()` inherits the mapping. `sem_init(..., 1, ...)` creates process-shared semaphores. The writer waits for an empty slot and posts full, and the reader waits for full and posts empty. Both detach with `shmdt()`. | Both processes access the same payload storage without sending the record through a kernel queue. | Synchronization is explicit. This one-slot design requires a handoff for every record. |
| TCP socket | The server uses `socket()`, `bind()`, `listen()`, and `accept()`. The client calls `connect()`. The peers loop over `send()` and `recv()`; the client calls `shutdown(SHUT_WR)` and the server checks EOF. | A standard stream interface that can also connect across hosts. | More setup and protocol overhead. This demonstration binds only to IPv4 loopback. |

A pipe, FIFO, or TCP receive call may return fewer bytes than requested. The program collects exactly `sizeof(IPCMessage)` bytes for each record. That fixed layout is appropriate for these same-build, same-host processes. Interoperable network programs typically serialize fields explicitly. The queue already preserves record boundaries, while semaphore handoffs preserve them in shared memory.

For shared memory, the parent uses `shmctl(IPC_RMID)` after attaching, which marks the segment for deletion once both processes detach. The semaphores live inside that mapping and are destroyed by the parent after the child exits. The semaphore's `pshared` argument and its placement in shared memory are both necessary.

## Benchmark methodology and interpretation

- Each mechanism gets the same record format and payload bytes, with sequential IDs. A fresh transport and child are created for each trial.
- The parent starts `clock_gettime(CLOCK_MONOTONIC, ...)` after endpoint setup and the child's readiness signal. It stops timing when it receives the child's verified count. Timing includes sending, receiving, checking every record, the final control-pipe acknowledgement, and any protocol-specific end-of-transfer check. Setup, child reaping, resource cleanup, and CSV output are outside the timing.
- The benchmark prints arithmetic mean elapsed milliseconds. Throughput equals all verified messages divided by the sum of measured trial durations in seconds. Verified counts are totals across trials. Failed runs leave the previous complete CSV in place.
- The ranking uses measured throughput for the selected configuration. These are end-to-end one-way transfer measurements. Timing for the interactive demos includes printing. A loopback TCP result describes local TCP, and the one-slot shared-memory result describes a semaphore handoff per record. CPU scheduling, Docker virtualization, and background load can change the order.

On a fresh checkout, option 7 displays the qualitative comparison. Run option 6 to generate a local CSV and add measured results to the report. Each completed benchmark replaces the CSV.

All five mechanisms exchange real records between separate processes. Comparing the verified runs shows how stream framing, kernel queuing, shared-memory synchronization, and connection setup affect the design of an IPC program.
