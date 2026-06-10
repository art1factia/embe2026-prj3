# Project#3 - Task 1. Cosmos+ OpenSSD KV-SSD Benchmark
*Sogang University – Embedded Systems Software (CSE4116, Prof. Youngjae Kim)*

---

This benchmark verifies whether the **Key-Value SSD (KV-SSD) firmware implementation** in Cosmos+ OpenSSD works well by performing thousands of direct NVMe key-value PUT/GET operations through the `ioctl` passthrough interface and checking whether the device always returns the _most recent value_ for every key.

---

## Build

```bash
make
````

This compiles `kv_bench` using `nvme_passthru.cc` and `nvme_passthru.h`.

---

## Run the Benchmark

### Arguments

| Position | Meaning                                        | Example        |
| -------- | ---------------------------------------------- | -------------- |
| 1        | NVMe device node                               | `/dev/nvme1n1` |
| 2        | Number of PUT operations                       | `10000`        |
| 3        | Keyspace size (range of random key generation) | `4096`         |
| 4        | NVMe device NSID                               | `1`            |

Example:

```bash
sudo ./kv_bench /dev/nvme1n1 10000 4096 1
```

---

## Output Example

```
[INFO] GET failed @ key `{` (No such key)
-----------------------------------------------
 Cosmos+ OpenSSD-Based KV-SSD Benchmark
-----------------------------------------------
 ops=10000 keyspace=4096
 result: OK=3746 FAIL=0 NO-SUCH-KEY=1
 elapsed: 2486.43 ms  (8043.67 IOPS est. for PUT+GET )
-----------------------------------------------
```

- OK: Count of keys whose GET value exactly matched the last PUT value.
- FAIL: Mismatched values, missing writes, corrupted returns, or unexpected device behavior.
- NO-SUCH-KEY: Correct handling of a known non-existent key (expected behavior for KV-SSD).

---

## What It Does

1. **Random KV-PUT operations**
  - Generates random 4-byte keys within a user-defined keyspace.
  - Some keys intentionally overlap to test “latest-write-wins” semantics.
  - Stores the most recent value for each key on the host side.
2. **Single GET on a non-existing key**
  - Confirms the SSD returns a “No such key” NVMe status correctly.
3. **KV-GET verification**
  - Reads each previously written key.
  - Compares the returned value with the host-side “latest value” map.
  - Reports any mismatch as a KV-SSD correctness failure.

---

## Precautions

* **Do NOT run on mounted or active filesystems.**
* The program directly overwrites LBAs on the target NVMe device.
* It uses only user-level I/O via `ioctl`.
* Stop the benchmark anytime with `Ctrl+C` (progress will stop gracefully).

---

## Clean Up

```bash
make clean
```

Removes the compiled binary and object files.

---

