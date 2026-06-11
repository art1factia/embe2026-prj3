# CSE4116: Embedded Systems Software
# Project #3. Key-Value Interface Enablement and KV-SSD Firmware Implementation

## Objective

Modern storage systems are willing to adopt alternative abstractions beyond the traditional block interface. Among these emerging models, the **Key-Value (KV) interface** has gained attention as a next-generation storage abstraction. A device that supports this interface is referred to as a **Key-Value SSD (KV-SSD)**, where data is accessed directly by user-defined keys rather than fixed-size logical block addresses. The KV interface eliminates the need for filesystem-level indexing structures and reduces data movement by removing unnecessary block translations. Furthermore, it shifts part of the key-value management logic from the host software stack into the SSD controller, simplifying host-side software and enabling hardware-accelerated key-value operations.

In this project, the Cosmos+ OpenSSD platform is extended to support the KV interface. Building on the understanding of the existing firmware stack, the work includes: enabling the KV command path, implementing the necessary firmware routines to store and retrieve key-value pairs, and integrating them into the device's I/O processing pipeline. The end result must be a minimal yet functional KV-SSD firmware that can correctly process `PUT` and `GET` operations issued from the host, validated for correctness through a provided benchmarking tool.

## Duration

May 28 (Thu) – June 11 (Thu) (2 weeks)

## Task Description

### Task 1: Enabling the Key-Value Interface and Developing KV-SSD Firmware

- Study the provided host-side `kv_bench` program and analyze how it issues NVMe KV commands through the `nvme_passthru.{h|cc}` interfaces. Specifically, understand the exact command format adopted in this assignment and how keys and values are transferred over the NVMe I/O path.

- Based on the KV command format, design and implement the corresponding KV handlers inside the Cosmos+ firmware. (**Hint**: revisit the implementation from Project #1 and recall how custom commands were registered and dispatched in the firmware.)

- Once the interface is enabled, construct an in-device indexing structure for KV storage and lookups. The choice of data structure is entirely open (e.g., hash table, LSM-tree, B+-tree, or any hybrid design), and in-memory and NAND-backed components may be freely combined as needed.

- Design the mechanism for storing and managing values on NAND flash. Think carefully about how the firmware should allocate, track, and update value locations across KV `PUT` and `GET` operations.

### Optional Development Direction (recommended starting point if unsure where to begin)

- Implement the index fully in memory for the initial version. Before allocating memory, review `memory_map.h` to understand how physical memory is organized in the OpenSSD environment. Since the system has no operating system supported, all memory must be statically reserved and manually managed.

- For value storage, recall the fundamental purpose of the Flash Translation Layer (FTL): *to provide a logical address space backed by NAND physical blocks to users*. The firmware itself is also a *user* of that logical space. A simple design is to maintain **an append-only write log in the logical address space**, where each `PUT` appends a new value and the index tracks its current location.

(Reference figure in the spec: an Append-Only Value Log placed on a Logical Linear Address Space, which sits on top of the FTL — the FTL internally maintains the page-level mapping table and garbage collection over NAND Flash.)

## Checklist

- **Modifiable scope**: Any firmware source file under `GreedyFTL/cosmos_app/src/` may be freely modified, and new source/header files may be created if needed.

- The firmware must correctly expose the KV interface to the host:
  - The provided host benchmark program (`kv_bench`) should successfully issue NVMe KV-PUT and KV-GET operations using the passthrough interface.
  - The firmware must decode the NVMe KV command format adopted in this assignment and invoke the appropriate KV handlers **without causing device aborts or assertion failures**.

- The KV-SSD firmware must implement the minimum functionality required for correctness:
  - A working `PUT` path that stores the value and updates an index structure.
  - A working `GET` path that retrieves the **most recent** value for a given key.

- To pass this task, the host benchmark must:
  - Start and complete without device aborts or firmware crashes.
  - Successfully detect **"No such key" semantics** for a known absent key.
  - Print a result summary such as `result: OK=... FAIL=... NO-SUCH-KEY=... elapsed=...` following the expected output format described in the `README.md`.

- **Data mismatches (FAIL) are penalized.**

## Materials Provided

- **Host-side evaluation program** (`prj3-host-evaluation/`): Includes `Makefile`, `kv_bench.cc`, `nvme_passthru.(h|cc)`, and `README.md`. Running this binary verifies the firmware by issuing random KV I/O workloads.

## Student Submission

Submit the following items under the naming convention below:

1. `workspace/GreedyFTL/cosmos_app/src/` directory (e.g., `prj3-ssd-{studentID}.tar.gz`)
2. Project Report (exported to PDF) (e.g., `prj3-document-{studentID}.pdf`)

**Submission Window**: Opens May 28 (Thu) 15:00 / Closes Jun 11 (Thu) 23:59

## Evaluation Criteria

- **Functionality (60%)** — Firmware must successfully pass evaluations using `kv_bench`. The benchmark must complete without crashes or hangs, and the final result must report OK for all operations with **no FAIL entries**.
  - Runs **outside the default parameters** will also be used for evaluation. The default parameters use a keyspace size of 4096, with 10000 number of operations — i.e., `(10000, 4096)`. The maximum values used for evaluation will be `(1000000, 4194304)` for the parameters.

- **Report Quality (40%)** — clarity, explanation of design, limitations, and improvement plans.

### Clarification from Course Announcement (regarding the 10,000,000-operation test)

- **The full-score criterion is that 1,000,000 operations complete without any error** — i.e., the `(1000000, 4194304)` parameter set stated above is the maximum used for functionality grading.

- The **10,000,000-operation run requested in the report is an additional task**. Running it as-is **will produce FAIL entries — this is expected, and no points are deducted** for those failures.

- However, it is possible to make the 10,000,000-operation run succeed, in which case:
  1. **Keep the lookup/search logic itself unchanged**, but modify part of the `kv_bench` file.
  2. Modify the code **taking hardware constraints into account (hint: memory constraints)**.
  - If this route is taken, **the modified `kv_bench` file must also be submitted** along with the other deliverables.

## Notes

- Use administrative privileges (`sudo`) only when absolutely necessary, and always double-check before executing commands.
- Handle the Cosmos+ OpenSSD board with care. Physical mishandling can permanently damage the hardware.
- Any destructive damage to the board or the Linux development machine — whether physical or software-induced — is entirely the student's responsibility. Such incidents may delay progress, but all delays and losses during project execution must be borne by the student alone.

## References

[1] Yanqin Jin, Hung-Wei Tseng, Yannis Papakonstantinou, and Steven Swanson. "KAML: A flexible, high-performance key-value SSD." *2017 IEEE International Symposium on High Performance Computer Architecture (HPCA)*, pages 373–384, 2017.

[2] Chang-Gyu Lee, Hyeongu Kang, Donggyu Park, Sungyong Park, Youngjae Kim, Jungki Noh, Woosuk Chung, and Kyoung Park. "iLSM-SSD: An intelligent LSM-tree based key-value SSD for data analytics." *2019 IEEE 27th International Symposium on Modeling, Analysis, and Simulation of Computer and Telecommunication Systems (MASCOTS)*, pages 384–395, 2019.

[3] Chanyoung Park, Jungho Lee, Chun-Yi Liu, Kyungtae Kang, Mahmut Taylan Kandemir, and Wonil Choi. "AnyKey: A key-value SSD for all workload types." *Proceedings of the 30th ACM International Conference on Architectural Support for Programming Languages and Operating Systems, Volume 1 (ASPLOS '25)*, pages 47–63, 2025.

[4] Junhyeok Park, Chang-Gyu Lee, Soon Hwang, Soonyeal Yang, Jungki Noh, Woosuk Chung, Junghee Lee, and Youngjae Kim. "BandSlim: A novel bandwidth and space-efficient KV-SSD with an escape-from-block approach." *Proceedings of the 53rd International Conference on Parallel Processing (ICPP '24)*, pages 1187–1196, 2024.
