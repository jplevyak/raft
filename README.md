# Raft Consensus Library

This is a C++ implementation of the Raft consensus algorithm, designed for robustness and flexibility. It handles leader election, log replication, and configuration changes (using Joint Consensus) to ensure consistency across a distributed cluster.

## Features

- **Leader Election**: Dynamic election of a cluster leader to coordinate log replication.
- **Log Replication**: Reliable replication of state machine commands to followers.
- **Snapshotting**: Support for log compaction via snapshots.
- **Configuration Updates**: Safe, arbitrary cluster topology changes (node addition/removal) using **Joint Consensus** (Safety verified).
- **Failure Resistance**: Tolerates network partitions, message loss, and node failures.

## Building and Testing

The project includes a `makefile` for easy compilation and testing.

### Build
Run `make` to build the test binary:
```bash
make
```

### Test
Run the comprehensive test suite (powered by `googletest`):
```bash
make test
```
This runs all 31+ regression tests covering leader election, log replication, and complex configuration changes.

### Example Server (KV Store)
An example Key-Value store server is provided in `example_server.cc`. It uses standard POSIX I/O for the Write-Ahead Log (WAL) and TCP sockets for communication.

**Build**:
```bash
make example_server
```

**Run Manual Cluster**:
To run a 3-node cluster manually on localhost:
```bash
# Terminal 1
./example_server 5001 5001 127.0.0.1:5002 127.0.0.1:5003

# Terminal 2
./example_server 5002 5002 127.0.0.1:5001 127.0.0.1:5003

# Terminal 3
./example_server 5003 5003 127.0.0.1:5001 127.0.0.1:5002
```

**Client Interaction**:
The server listens for client connections on `port + 1000` (e.g., 6001 for node 5001).
Protocol is simple newline-delimited text:
- `set key value` -> Returns `OK` or `NOT_LEADER <leader_host:port>`
- `get key` -> Returns `value` or `NOT_FOUND`

**End-to-End Test**:
A python script is included to automatically spin up a cluster, perform writes, verify replication, and test persistence across restarts.
```bash
python3 test_example_server.py
```

## Getting Started

### Prerequisites

- **C++17 compliant compiler** (e.g., `g++` or `clang++`)
- **Protocol Buffers** (`protobuf`)
- **Google Test** (`gtest`)

#### Ubuntu Installation
You can install the required dependencies using `apt`:
```bash
sudo apt-get install libgtest-dev libprotobuf-dev protobuf-compiler
```
(Note: Package names may vary by distribution, shorter aliases like `protobuf-dev` or `gtest-dev` might exist on some systems).

### Integration

1.  **Include the Header**:
    ```cpp
    #include "raft.h"
    ```

2.  **Implement the Server Interface**:
    You must create a class that implements `raft::RaftServerInterface`. This class acts as the bridge between the Raft logic and your application's networking and storage.

    ```cpp
    class MyServer : public raft::RaftServerInterface {
    public:
        // Transport: Send a message to another node
        bool SendMessage(RaftClass *raft, const std::string &node, const Message &message) override {
            // Implement your network send logic here
        }

        // Storage: Retrieve a log entry
        void GetLogEntry(RaftClass *raft, int64_t term, int64_t index, int64_t end, LogEntry *entry) override {
            // Read from your persistent log
        }

        // Storage: Persist a log entry
        void WriteLogEntry(RaftClass *raft, const LogEntry &entry) override {
            // Write to your persistent log (ensure fsync!)
        }

        // State Machine: Apply a committed entry
        void CommitLogEntry(RaftClass *raft, const LogEntry &entry) override {
            // Apply entry.data() to your state machine
        }

        // Callbacks
        void LeaderChange(RaftClass *raft, const std::string &leader) override { ... }
        void ConfigChange(RaftClass *raft, const Config &config) override { ... }
    };
    ```

3.  **Instantiate Raft**:
    ```cpp
    MyServer server;
    raft::Raft<MyServer>* raft_node = raft::NewRaft(&server, "node_id_1");
    ```

4.  **Run the Main Loop**:
    Your application drives the Raft instance:
    *   **Time**: Call `raft_node->Tick(now)` periodically (e.g., every 10-50ms).
    *   **Messages**: When a message arrives from the network, pass it to `raft_node->Run(now, message)`.

## API Overview

### `raft::Raft` Class

-   `Start(double now, int64_t random_seed)`: Initialize and start the Raft node.
-   `Propose(const LogEntry &entry)`: Propose a new command (data or config change) to be replicated.
-   `Tick(double now)`: Periodic clock tick for timeouts and heartbeats.
-   `Run(double now, const Message &message)`: Process an incoming message.
-   `Snapshot(...)`: Create a snapshot of the current state.
-   `Stop()`: Cleanly shut down the node.

### `raft::RaftServerInterface`

Your server implementation is responsible for:
-   **Networking**: Delivering protobuf messages (`RaftMessagePb`) between nodes.
-   **Storage**: Durably storing the Raft log (`RaftLogEntryPb`). **Critical:** `WriteLogEntry` must be synchronous and durable (fsync) before returning to ensure safety.
-   **State Machine**: Applying committed commands deterministically.

## Configuration Changes (Joint Consensus)

This implementation uses **Joint Consensus** to handle arbitrary configuration changes (e.g., replacing multiple nodes at once). This ensures safety even during complex topology shifts where the "old" and "new" quorums might be disjoint.

To change the cluster configuration, create a `LogEntry` with a `RaftConfigPb` and pass it to `Propose()`.

## Protocol Buffers

The data structures are defined in `raft.proto`:
-   `RaftLogEntryPb`: Represents a single log entry (Team, Index, Data, Config).
-   `RaftMessagePb`: Represents a wire message (VoteRequest, AppendEntries, etc.).
-   `RaftConfigPb`: detailed configuration of participating nodes.

> [!WARNING]
> **Thread Safety**: The `Raft` class is **not thread-safe** and is not reentrant. You must wrap calls to the Raft object (like `Tick`, `Run`, `Propose`) with your own locking mechanism if accessing it from multiple threads.

## License

This project is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Please ensure any changes are accompanied by regression tests in `raft_test.cc`. Run `make test` to verify your changes.
