# `ctrace` — AST-Driven Zero-Boilerplate Tracing System for C

[![CI](https://github.com/bpodrygajlo/ctrace/actions/workflows/ci.yml/badge.svg)](https://github.com/bpodrygajlo/ctrace/actions/workflows/ci.yml)
[![Standard](https://img.shields.io/badge/C-C11%20%2F%20C17%20%2F%20C23-blue.svg)]()
[![Tooling](https://img.shields.io/badge/LLVM%2FClang-17%20%7C%2018-orange.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)]()

`ctrace` provides a modern, Rust-style zero-boilerplate tracing experience for C applications. Developers instrument code simply by passing an event name and variables:

```c
TRACE_EVENT(net_rx, socket_fd, len, payload);
```

Behind the scenes, a standalone Clang `LibTooling` compiler pass inspects the AST, extracts canonical C types, enums, and recursive struct layouts, and generates strongly-typed, zero-overhead C serializers for multiple configurable backends.

---

## Key Features

* **Ergonomic API:** Single-line instrumentation with zero format strings, zero manual type specifiers, and no separate schema definition files.
* **Zero-Syscall Hardware Timestamps:** Avoids `CLOCK_REALTIME` kernel traps by sampling CPU cycle registers directly (`rdtsc` on x86_64, `cntvct_el0` on ARM64) in ~6–8 ns, converting to nanoseconds via fixed-point multiplication without runtime integer divisions.
* **Compile-Time Backend Switching:** Switch backends with a single CMake flag (`-DCTRACE_BACKEND=...`) without changing a single line of C source code:
  * **`stdout`**: Thread-safe (`flockfile`) line-delimited logging with monotonic timestamps.
  * **`bin`**: Packed little-endian binary stream powered by a lock-free per-thread SPSC circular ring buffer (256 KB) and background writer thread.
  * **`bin-zstd`**: Chunked 64 KB Zstandard-compressed binary stream with amortized frame headers.
  * **`sqlite`**: Direct-to-file SQLite database backend (`trace.db`) with write-optimized PRAGMAs (`synchronous=OFF`, `locking_mode=EXCLUSIVE`, `journal_mode=MEMORY`), automatic per-event DDL table creation, and transaction batching for post-run SQL analysis.
  * **`ctf`**: Common Trace Format (CTF 1.8) stream with auto-generated TSDL metadata, directly readable by **`babeltrace2`**, **`babeltrace`**, and **Eclipse Trace Compass**.
* **Cross-TU Validation:** Incremental per-file AST sidecar extraction with cross-translation-unit signature validation (aborts on type/arity mismatches with exact file and line diagnostics).

---

## Architecture Overview

`ctrace` operates as a two-phase build pipeline integrated directly into CMake:

```text
  [ user_app.c ] ──(1. Scan Pass: -DCTRACE_AST_SCAN)──► [ ctrace-scan (Clang AST) ]
                                                                    │
                                                          emits user_app.c.trace.json
                                                                    ▼
  [ trace_generated.h ] ◄──(2. Merge & Codegen)────────── [ ctrace-gen --backend=... ]
  [ trace_schema.json ]
          │
          ▼
  [ C Compiler (gcc/clang) ] ──(3. Normal Compile + Link libctrace_rt)──► [ Binary ]
```

1. **Phase 1 (AST Scan):** During the scan pass, `TRACE_EVENT` expands to a dummy variadic hook `__ctrace_hook(event_name, ...)`. Clang parses the TU with full include paths, resolving typedefs, enum constants, and struct layouts. `ctrace-scan` emits `<obj>.trace.json`.
2. **Phase 2 (Merge & Codegen):** `ctrace-gen` validates signature consistency across all translation units, assigns deterministic 16-bit event IDs, and generates `trace_generated.h` tailored to the selected backend.
3. **Phase 3 (Native Compile):** The native compiler compiles the application with `trace_generated.h` on the include path and links the corresponding `libctrace_rt_<backend>` runtime library.

---

## Multithreading Architecture & Concurrency Model

Modern multicore tracing overhead is dominated by shared cache invalidations and lock contention. `ctrace` eliminates these using a **Single-Producer Single-Consumer (SPSC)** thread-isolated topology:

```text
┌─────────────────┐       ┌──────────────────────────────┐
│  Worker Thread  │──────►│  Thread-Local Ring Buffer    │ (256 KB private buffer in L1D)
│    (Core X)     │       │  Zero cross-core contention  │
└─────────────────┘       └──────────────┬───────────────┘
                                         │ atomic write_pos (Release)
                                         ▼
                          ┌──────────────────────────────┐
                          │    ctrace-writer Thread      │ (Background drainer / 2ms loop)
                          │   (Dedicated/Housekeeping)   │
                          └──────────────┬───────────────┘
                                         │ Page write / fsync
                                         ▼
                                  [ trace.bin ]
```

* **Zero Cross-Core Contention:** Every thread allocates a private, lock-free **256 KB circular buffer** (`_Thread_local`). Workers execute zero Compare-And-Swap (`lock cmpxchg`) loops and zero atomic fetch-and-adds. No two CPUs ever write to the same cache lines.
* **What Happens When a Thread Falls Behind? (Backpressure Protocol):**
  If a thread produces events faster than disk I/O can drain, it follows a 3-step saturation policy:
  1. **Space Check:** Computes available buffer space against atomic `read_pos`.
  2. **Micro-Yield:** If space is exhausted, performs a brief 100 µs spin/yield giving the writer thread an immediate window to flush pending pages.
  3. **Non-Blocking Drop (Never Halt the Application):** If the buffer remains saturated, `ctrace` **drops the event immediately** rather than stalling the application's critical path.
* **Real-Time Guard:** If `ctrace_init()` is called from a real-time thread (`SCHED_FIFO` / `SCHED_RR`), `ctrace` automatically decouples the background drainer to standard `SCHED_OTHER` so it never interferes with real-time tasks.
* **Housekeeping Core Isolation:** The background thread can be pinned to a housekeeping CPU core (`ctrace_set_writer_affinity(0)` or `CTRACE_WRITER_CPU=0`) and lowered in nice priority (`CTRACE_WRITER_PRIORITY=10`).
* **Robust Error Handling:** Diagnostic and tuning calls (`pthread_setname_np`, `pthread_setaffinity_np`, `setpriority`) are strictly best-effort and never crash. Lifecycle calls (`pthread_create`, `pthread_join`) are verified to prevent uninitialized thread joins.

---

## Quick Start

### 1. Instrument Your Code

```c
// main.c
#include "ctrace/trace.h"

typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

typedef struct {
    int id;
    float ratio;
} Metric;

int main(void) {
    // Initialize backend (e.g., NULL/path for stdout/bin/ctf, ":memory:" for sqlite)
    ctrace_init(NULL);

    int socket_fd = 42;
    size_t len = 512;
    const char *data = "payload_content";
    ctrace_bytes_t payload = TRACE_BYTES(data, 15);
    LogLevel lvl = LOG_INFO;
    Metric m = { .id = 101, .ratio = 0.95f };

    // Single-line instrumentation
    TRACE_EVENT(net_rx, socket_fd, len, payload);
    TRACE_EVENT(health_check, lvl, m);

    ctrace_flush();
    ctrace_shutdown();
    return 0;
}
```

### 2. Configure CMake

#### Option A: Installed Package (`find_package`)
```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(my_service C)

find_package(ctrace REQUIRED)

add_executable(my_service main.c)
target_enable_ctrace(my_service BACKEND bin)
```

#### Option B: CPM.cmake (CMake Package Manager)
```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(my_service C)

include(cmake/CPM.cmake)

CPMAddPackage(
    NAME ctrace
    GITHUB_REPOSITORY bpodrygajlo/ctrace
    GIT_TAG main # or SOURCE_DIR /path/to/ctrace
)

add_executable(my_service main.c)
target_enable_ctrace(my_service BACKEND bin)
```

### 3. Build & Switch Backends

Build with default `stdout` backend:
```bash
cmake -B build -S .
cmake --build build
./build/my_service
```

Switch to **Lock-Free Binary** (`bin`):
```bash
cmake -B build -S . -DCTRACE_BACKEND=bin
cmake --build build
./build/my_service
python3 /path/to/ctrace/tools/ctrace_decode.py build/ctrace_gen_my_service/trace_schema.json trace.bin
```

Switch to **CTF (Common Trace Format)**:
```bash
cmake -B build -S . -DCTRACE_BACKEND=ctf
cmake --build build
./build/my_service
babeltrace2 trace_ctf
```

Switch to **SQLite Database** (optional, requires SQLite3):
```bash
cmake -B build -S . -DCTRACE_BACKEND=sqlite
cmake --build build
./build/my_service
sqlite3 trace.db "SELECT * FROM ctrace_net_rx LIMIT 10;"
```

Switch to **Zstandard-Compressed Binary** (optional, requires Zstandard):
```bash
cmake -B build -S . -DCTRACE_BACKEND=bin-zstd
cmake --build build
./build/my_service
python3 /path/to/ctrace/tools/ctrace_decode.py build/ctrace_gen_my_service/trace_schema.json trace.bin.zst
```

### 4. Optional External Dependencies & Configuration

`ctrace` does not force heavy dependencies on your build environment:

| Backend | External Dependency | CMake Control Option | Default |
| :--- | :--- | :--- | :--- |
| **`stdout`** | *None* (pure C / POSIX) | Always enabled | Built-in |
| **`bin`** | *None* (pure C lock-free SPSC) | Always enabled | Built-in |
| **`ctf`** | *None* (pure C CTF 1.8 packetizer) | Always enabled | Built-in |
| **`bin-zstd`** | `libzstd` | `-DCTRACE_ENABLE_ZSTD=ON/OFF` | `ON` (auto-disabled if missing) |
| **`sqlite`** | `libsqlite3` | `-DCTRACE_ENABLE_SQLITE=ON/OFF` | `ON` (auto-disabled if missing) |

- **Zero-Dependency Core:** `stdout`, `bin`, and `ctf` require **no external C libraries**. They build cleanly in stripped-down embedded and container environments.
- **Graceful Fallback:** If `libzstd` or `libsqlite3` is absent, CMake logs an informative note (`ctrace: SQLite3 not found -> 'sqlite' backend disabled`) and continues without failing.
- **Backend Selection Hierarchy:**
  1. **Per-Target Override:** `target_enable_ctrace(my_target BACKEND <backend>)` explicitly controls which runtime backend links to that specific executable.
  2. **Global Default:** If `BACKEND` is omitted, `target_enable_ctrace()` defaults to the CMake cache variable `-DCTRACE_BACKEND=<backend>` (default: `stdout`).
  3. **Safety Validation:** If an application requests a backend whose external library was disabled or not found, CMake emits an immediate, actionable error with resolution instructions.

### Dynamic Wire Formats & Nested Arrays (`CTRACE_COUNTED_BY`)

`ctrace` seamlessly parses and emits multi-layer dynamic packet structures (including C99 Flexible Array Members and dynamically allocated pointer arrays) by binding payload arrays to their length/counter fields:

```c
#include "ctrace/ctrace_defs.h"

// Layer 2: Sub-item with dynamic payload
typedef struct {
    int sensor_id;
    uint16_t num_readings;
    float *readings CTRACE_COUNTED_BY(num_readings);
} SensorData;

// Layer 1: Batch with dynamic sub-items
typedef struct {
    uint32_t batch_id;
    uint16_t num_sensors;
    SensorData *sensors CTRACE_COUNTED_BY(num_sensors);
} TelemetryBatch;

// Single-layer wire frame with Flexible Array Member (FAM)
typedef struct {
    uint16_t msg_type;
    uint16_t num_items;
    int items[] CTRACE_COUNTED_BY(num_items);
} WireFrame;

#include "ctrace/trace.h"

// Passing complex structures by value or pointer:
TRACE_EVENT(telemetry_batch, batch);
TRACE_EVENT(wire_frame, frame);
// Formatted output:
// batch={batch_id=999, num_sensors=2, sensors=[{sensor_id=101, num_readings=2, readings=[1.5, 2.5]}, ...]}
// frame={msg_type=77, num_items=3, items=[100, 200, 300]}
```

### Third-Party Headers & Discriminated Unions (`CTRACE_BIND_COUNT`, `CTRACE_BIND_UNION`)

Vendor headers (e.g. FAPI/nFAPI message definitions) cannot be annotated in place. Bindings declared once in any `.c` file attach the same information out of line:

```c
// Vendor header (unchanged)
typedef struct { uint16_t num_pdus; fapi_pdu_t pdus[64]; } fapi_dl_tti_req_t;
typedef struct {
    uint16_t pdu_type;
    union { fapi_dlsch_pdu_t dlsch; fapi_pdcch_pdu_t pdcch; } u;
} fapi_pdu_t;

// Your .c file
CTRACE_BIND_COUNT(fapi_dl_tti_req_t, pdus, num_pdus);        // only num_pdus elements are serialized
CTRACE_BIND_UNION(fapi_pdu_t, pdu_type, 0, dlsch);           // pdu_type == 0  -> u.dlsch
CTRACE_BIND_UNION(fapi_pdu_t, pdu_type, 1, pdcch);           // pdu_type == 1  -> u.pdcch
CTRACE_BIND_UNION(fapi_pdu_t, pdu_type, 2, dlsch);           // several tags may select one member

TRACE_EVENT(dl_tti, req);
// stdout: req={num_pdus=1, pdus=[{pdu_type=1, u={pdcch={freq_domain=43981, aggregation_level=4}}}]}
```

* The tag field is a sibling of the union; tag values may be enum constants, negative numbers or full-range unsigned values.
* Anonymous unions, several unions per struct (each with its own tag) and unions reached through a struct pointer are supported.
* A tag value without a binding, or a union without any binding (e.g. `pthread_mutex_t`), is captured as the raw `sizeof(union)` bytes and shown as hex.
* On the wire (`bin`, `ctf`, `sqlite` BLOB) a union is `uint32_t len` followed by the serialized active member; decoders select the member from the tag field.
* Fields that carry no information, such as a `pthread_mutex_t` inside a vendor struct, are dropped with `CTRACE_BIND_SKIP(type, field)` (or `CTRACE_SKIP` on the field itself in your own headers).

---

## API Reference

### Unified Lifecycle & Macro API (`ctrace/trace.h`)

| Function / Macro | Description |
| :--- | :--- |
| `TRACE_EVENT(name, ...)` | Instruments an event with arbitrary local variables, expressions, or structs. |
| `TRACE_BYTES(ptr, len)` | Constructs a `ctrace_bytes_t` fat-pointer payload for raw buffers. |
| `CTRACE_COUNTED_BY(field)` | Struct attribute binding a dynamic pointer (`T*`) or FAM (`T[]`) to an integer counter field. Supports arbitrary multi-layer nesting. |
| `CTRACE_BIND_COUNT(type, array, count)` | Out-of-line equivalent of `CTRACE_COUNTED_BY` for structs whose header cannot be edited; also binds fixed arrays (`T[N]`). |
| `CTRACE_BIND_UNION(type, tag, value, member)` | Declares that `member` of the union inside `type` is active when the sibling field `tag` equals `value`. |
| `CTRACE_SKIP` / `CTRACE_BIND_SKIP(type, field)` | Leaves a struct field out of every trace (locks, caches, opaque handles), in the header or out of line. |
| `void ctrace_init(const char *target_uri)` | Initializes the runtime (`NULL` for default, directory for CTF, file for binary, `":memory:"` for SQLite). |
| `void ctrace_flush(void)` | Flushes pending events in ring buffers, chunks, or transactions to disk. |
| `void ctrace_shutdown(void)` | Flushes and terminates writer threads, closes file handles or databases. |

### Low-Latency Hardware Timers

| Function | Description |
| :--- | :--- |
| `ctrace_now_cycles()` | Inlined hardware cycle counter (`rdtsc` / `cntvct_el0`) with ~6–8 ns overhead. |
| `ctrace_now_ns()` | Returns monotonic nanoseconds using fixed-point cycle math (no syscalls, zero integer division). |
| `ctrace_get_tsc_freq()` | Returns the calibrated hardware clock frequency in Hz. |
| `ctrace_cycles_to_ns(cycles)` | Converts raw hardware cycle counter values to nanoseconds. |

### Background Writer Thread Controls (`bin` backend)

| Function / Env Var | Description |
| :--- | :--- |
| `void ctrace_set_writer_affinity(int cpu_core)` | Pin the background drainer thread to a designated housekeeping CPU core. |
| `void ctrace_set_writer_priority(int nice_val)` | Set the nice priority of the background drainer thread (e.g. `+10` or `+19`). |
| `CTRACE_WRITER_CPU=<core>` | Environment variable setting writer CPU affinity without recompilation. |
| `CTRACE_WRITER_PRIORITY=<nice>` | Environment variable setting writer nice level without recompilation. |

> [!NOTE]
> **Thread Naming & Real-Time Guard:** The background thread is automatically named `"ctrace-writer"` (visible in `top`, `htop`, `ps -T`). If `ctrace_init()` is called from a real-time thread (`SCHED_FIFO` / `SCHED_RR`), `ctrace` automatically decouples the writer to standard `SCHED_OTHER` so it never pollutes the real-time scheduling class.

---

## Type Mapping Matrix

| C Type (Clang Canonical) | `stdout` | `bin` / `bin-zstd` | `sqlite` | `ctf` |
| :--- | :--- | :--- | :--- | :--- |
| **Integers (`int*`, `uint*`)** | `%d`, `%lld`, `%zu` | Little-endian `memcpy` | `INTEGER` (`bind_int64`) | `intN_t` / `uintN_t` |
| **`float` / `double`** | `%.6g` | IEEE-754 `memcpy` | `REAL` (`bind_double`) | `float` / `double` |
| **`bool` (`_Bool`)** | `"true"` / `"false"` | `uint8_t` (0 or 1) | `INTEGER` (0 or 1) | `uint8_t` |
| **`enum`** | Symbolic name | `int32_t` | `INTEGER` | CTF `enum : int32_t` |
| **`const char *`** | `"%s"` (null-safe) | `uint32_t len` + bytes | `TEXT` (`bind_text`) | CTF `string` (null-term) |
| **`ctrace_bytes_t`** | Hex preview | `uint32_t len` + bytes | `BLOB` (`bind_blob`) | `uint32_t len` + dynamic array |
| **`struct`** | Flattened `field_sub=val` | Recursive serialization (skips pointers) | Flattened columns (`field_sub`) | Flattened struct fields |
| **`T[N]` scalars (N > 4, or passed directly)** | Hex preview | Raw `memcpy` of the array | `BLOB` | `uint8_t name[N]` |
| **`struct T[N]` (N > 4)** | `name=[{...}, ...]` | `uint32_t len` + N packed elements (no padding) | `BLOB` | `uint32_t len` + dynamic array |
| **`union` (bound)** | `u={member={...}}` | `uint32_t len` + active member | `BLOB` (active member) | `uint32_t len` + dynamic array |
| **`union` (unbound)** | Hex preview | `uint32_t len` + raw bytes | `BLOB` (raw bytes) | `uint32_t len` + dynamic array |

---

## Building and Installing `ctrace`

### Prerequisites

* Linux (x86_64, AArch64)
* CMake $\ge$ 3.20
* Clang / LLVM $\ge$ 17 (development libraries: `libclang-18-dev`, `llvm-18-dev`)
* **Optional Dependencies (all auto-detected with graceful fallback):**
  * `nlohmann-json3-dev` (optional; falls back automatically to built-in C++17 JSON implementation in `tools/common/ctrace_json.hpp`)
  * `libzstd-dev` (only required for `-DCTRACE_ENABLE_ZSTD=ON`)
  * `libsqlite3-dev` (only required for `-DCTRACE_ENABLE_SQLITE=ON`)
  * `babeltrace2` (optional test utility for CTF trace inspection)

> [!TIP]
> The core backends (`stdout`, `bin`, and `ctf`) and build tools require **zero external C/C++ libraries** beyond standard POSIX pthreads and Clang/LLVM.

On Ubuntu / Debian:
```bash
# Minimal install (core backends: stdout, bin, ctf — no external JSON/Zstd/SQLite libs needed):
sudo apt-get install -y cmake clang libclang-18-dev llvm-18-dev

# Full install (including optional SQLite and Zstandard backends):
sudo apt-get install -y cmake clang libclang-18-dev llvm-18-dev \
    libsqlite3-dev libzstd-dev babeltrace2
```

### Build & Run Test Suite

```bash
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
```

### Install System-Wide

```bash
sudo cmake --install build
```
This installs:
* Executables: `ctrace-scan`, `ctrace-gen`
* Runtime Libraries: `libctrace_rt_stdout.a`, `libctrace_rt_bin.a`, `libctrace_rt_bin-zstd.a`, `libctrace_rt_sqlite.a`, `libctrace_rt_ctf.a`
* Headers: `<ctrace/trace.h>`
* CMake Package: `find_package(ctrace REQUIRED)` integration via `ctrace.cmake`

---

## License

This project is licensed under the MIT License.
