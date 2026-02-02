# Raft Consensus Algorithm Implementation (C++)

A distributed, fault-tolerant transaction ledger implementation based on the **Raft Consensus Algorithm**. This project simulates a 3-node cluster that manages a consistent state machine (bank accounts) across a network using Windows Winsock.

---

## 🚀 Features

* **Leader Election:** Automatic transition between Follower, Candidate, and Leader roles.
* **Safety Guarantees:** Log freshness checks during elections to ensure only the most up-to-date node becomes leader.
* **Persistence:** State (term, log, votedFor) is saved to `node_ID_storage.txt` to survive node crashes.
* **Heartbeat Mechanism:** Leader maintains authority and synchronizes log commitment via periodic heartbeats.
* **State Machine:** Replicates transactions between "Alice" and "Bob" across all nodes.

---

## 🛠 Project Structure

### System Architecture
The following diagram illustrates how the Raft nodes interact within the cluster:



| Class | Responsibility |
| :--- | :--- |
| **`RaftNode`** | Manages the Raft State Machine, timers, and role transitions. |
| **`NetworkManager`** | Handles TCP socket communication (Winsock2) and broadcasting. |
| **`stable_storage`** | Handles file I/O for persistent data storage. |
| **`Transaction`** | The data unit containing sender, receiver, and amount. |

---

## 💻 Setup & Requirements

### Prerequisites
* **OS:** Windows (uses `winsock2.h` and `ws2tcpip.h`).
* **Compiler:** MinGW-w64 or MSVC.
* **Dependencies:** If using MinGW, ensure `mingw-std-threads` is in your include path for thread support.

### Compilation
Open your terminal and run:
```bash
./raft.exe
