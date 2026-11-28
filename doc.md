# Design Document: `ctrace` — AST-Driven Zero-Boilerplate Tracing System for C

**Status:** Draft  
**Target Language:** C11 / C17 / C23 (Tooling written in C++17 / Clang `LibTooling`)  
**Build System:** CMake ($\ge$ 3.20)  

---

## 1. Overview & Goals

`ctrace` provides a Rust-style, zero-boilerplate tracing experience for C applications. Developers instrument code by passing only an event name and local variables (`TRACE_EVENT(net_rx, socket_fd, len, payload)`). A standalone Clang `LibTooling` pass extracts variable names, canonical C types, enum definitions, and struct layouts directly from the Clang AST, generating strongly-typed, zero-format-parsing C serializers for multiple configurable backends.

### Key Goals
* **Ergonomic API:** Single-line instrumentation with zero manual type specifiers, format strings, or separate schema files.
* **Compile-Time Backend Selection:** Switch between `stdout`, `bin` (raw packed binary), `bin-zstd` (chunked Zstd-compressed binary), `sqlite` (in-memory `:memory:` SQLite database), and `ctf` (Common Trace Format for Babeltrace / Trace Compass) via a CMake flag (`-DCTRACE_BACKEND=...`).
* **Zero-Overhead Hot Path:** No runtime `printf` parsing or reflection; generates direct `memcpy` or `sqlite3_bind_*` calls. Zero-syscall hardware cycle counters (`rdtsc` / `cntvct_el0`) avoid `CLOCK_REALTIME` kernel traps.
* **Incremental Builds:** Per-translation-unit metadata extraction integrated natively into CMake dependency graphs.

---

## 2. System Architecture

`ctrace` operates as a two-phase build pipeline integrated into CMake:

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

### Phase 1: AST Extraction (`ctrace-scan`)
During the scan pass, `TRACE_EVENT` expands to a dummy variadic C function `__ctrace_hook(const char *event, ...)`. Clang parses the translation unit with full project include paths and macro definitions, resolving all typedefs, struct layouts, and enum constants. `ctrace-scan` visits every `CallExpr` to `__ctrace_hook` and writes a lightweight JSON sidecar (`<obj>.trace.json`).

### Phase 2: Schema Merge & Code Generation (`ctrace-gen`)
A fast standalone generator merges all `*.trace.json` files across the target:
1. Validates that events sharing the same name have identical arity and canonical types across translation units.
2. Assigns deterministic 16-bit `event_id`s.
3. Emits `trace_generated.h` containing `static inline` emitters tailored to the selected backend, plus `trace_schema.json` for offline binary decoding.

### Phase 3: Native Compilation
The standard C compiler compiles the application with `trace_generated.h` on the include path and links against `libctrace_rt` (the runtime ring buffer, Zstd compressor, or SQLite driver).

---

## 3. Frontend API & Type Mapping

### 3.1 User-Facing Header (`ctrace/trace.h`)
```c
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const void *ptr;
    size_t len;
} ctrace_bytes_t;

#define TRACE_BYTES(p, l) ((ctrace_bytes_t){ .ptr = (const void *)(p), .len = (size_t)(l) })

void ctrace_init(const char *target_uri); // e.g., NULL (stdout), "trace.bin", or ":memory:"
void ctrace_flush(void);
void ctrace_shutdown(void);

#ifdef CTRACE_BACKEND_SQLITE
int ctrace_sql_exec(const char *sql, int (*cb)(void*, int, char**, char**), void *arg);
int ctrace_sql_snapshot(const char *dest_path); // Wraps VACUUM INTO
#endif

#ifdef CTRACE_AST_SCAN
void __ctrace_hook(const char *event_name, ...);
#define TRACE_EVENT(name, ...) __ctrace_hook(#name, __VA_ARGS__)
#else
#include "trace_generated.h"
#define TRACE_EVENT(name, ...) __ctrace_emit_##name(__VA_ARGS__)
#endif
```

### 3.2 Variable Naming & AST Resolution Rules
For each argument `Expr` passed to `__ctrace_hook`:
* **Field Naming:**
  * `DeclRefExpr` (e.g., `socket_fd`) $\rightarrow$ uses exact identifier (`"socket_fd"`).
  * `MemberExpr` (e.g., `req->id`) $\rightarrow$ sanitizes to `"req_id"`.
  * Arbitrary expression (e.g., `a + b`) $\rightarrow$ falls back to `"arg_<index>"`.
* **Type Mapping Matrix:**

| Clang Canonical Type | `stdout` Codegen | `bin` / `bin-zstd` Codegen | `sqlite` Codegen |
| :--- | :--- | :--- | :--- |
| Signed/Unsigned Ints | `%d`, `%lld`, `%zu`, etc. | Raw `memcpy` (little-endian) | `INTEGER` (`sqlite3_bind_int64`) |
| `float` / `double` | `%.6g` | IEEE-754 `memcpy` | `REAL` (`sqlite3_bind_double`) |
| `bool` (`_Bool`) | `"true"` / `"false"` | `uint8_t` (`0` or `1`) | `INTEGER` (`0` or `1`) |
| `enum` | Generated `switch` $\rightarrow$ symbol name | Underlying `int32_t` | `INTEGER` (or `TEXT` option) |
| `const char *` / `char[N]` | `"%s"` (null-safe) | `uint32_t len` + UTF-8 bytes | `TEXT` (`sqlite3_bind_text`) |
| `ctrace_bytes_t` | Hex preview (`%02x...`) | `uint32_t len` + raw bytes | `BLOB` (`sqlite3_bind_blob`) |
| Struct (`RecordDecl`) | Flattened `field_sub=val` | Recursive field serialization (skips pointers) | Flattened columns (`field_sub`) |

---

## 4. Backend Specifications

### 4.1 `stdout` Backend
* **Mechanism:** Generates type-safe `fprintf(g_ctrace_out, ...)` calls with nanosecond monotonic timestamps (`clock_gettime(CLOCK_MONOTONIC)`).
* **Thread Safety:** Uses `flockfile` / `funlockfile` around each event line to prevent interleaved output across threads.

### 4.2 `bin` (Uncompressed Binary) Backend
* **Wire Format:**
  ```text
  [ uint16_t event_id ][ uint64_t timestamp_ns ][ packed field payloads... ]
  ```
* **Runtime (`libctrace_rt`):** Uses a per-thread lock-free SPSC ring buffer (default 256 KB per thread). A background writer thread drains full pages via `write()` / `io_uring`.
* **Schema Sidecar:** `trace_schema.json` records `event_id`, field names, byte widths, and endianness so external Python/Rust readers can unpack streams with zero framing overhead per field.

### 4.3 `bin-zstd` (Compressed Binary) Backend
* **Mechanism:** Uses the exact same binary emitter as `bin`, writing into a thread-local 64 KB staging chunk.
* **Compression:** When a 64 KB chunk fills (or `ctrace_flush()` is called), `ZSTD_compressCCtx()` compresses the chunk using a reusable thread-local `ZSTD_CCtx` and writes a framed block `[ uint32_t compressed_len ][ zstd_frame ]` to the output stream.
* **Benefit:** Amortizes Zstd frame headers across hundreds of events while keeping individual `TRACE_EVENT` calls in the ~30–80 ns range.

### 4.4 `sqlite` (Direct-to-File SQLite Database) Backend
* **Initialization (`ctrace_init("trace.db")`):**
  * Creates a fresh SQLite database file (defaults to `"trace.db"` when `NULL` or `""` is passed) with write-optimized settings: `PRAGMA synchronous=OFF; PRAGMA journal_mode=MEMORY; PRAGMA locking_mode=EXCLUSIVE; PRAGMA temp_store=MEMORY;`.
  * Executes generated `CREATE TABLE` DDL for every discovered event (e.g., `ctrace_<event_name>`).
  * Pre-compiles one `sqlite3_stmt *` per event (`INSERT INTO ctrace_<name> VALUES (?, ?, ...)`).
* **Hot-Path Execution:**
  * Calls `sqlite3_bind_*` directly for each argument, executes `sqlite3_step()`, and calls `sqlite3_reset()`.
  * **Transaction Batching:** Wraps inserts in an open transaction that commits automatically every $N$ events (default `1000`) or $T$ milliseconds (default `10 ms`) to eliminate per-event commit overhead.
  * **Post-Run SQL Investigation:** On `ctrace_shutdown()`, commits the final transaction, finalizes all prepared statements, and closes the database so users can query the resulting `.db` file directly with the `sqlite3` CLI or any SQL client.

### 4.5 `ctf` (Common Trace Format v1.8) Backend
* **Standard Compliance:** Emits 100% compliant CTF 1.8 packets directly readable by standard tools (`babeltrace2`, `babeltrace`, Eclipse Trace Compass).
* **Metadata Generation:** `ctrace-gen` compiles TSDL `metadata` with strict typed stream layouts, clock mapping, and enum symbolic mappings.
* **Packet Framing (`ctrace_rt_ctf`):** Fixed 64 KB packet chunks with 68-byte CTF packet headers (`0xC1FC1FC1`, UUID, `timestamp_begin`, `timestamp_end`, `content_size`, `packet_size`, `cpu_id`).

### 4.6 Hardware Cycle Counter & Low-Latency Timestamping
* **Zero Syscalls:** Avoids `CLOCK_REALTIME` kernel transitions. Reads CPU invariant TSC registers directly via `rdtsc` (x86_64) or `cntvct_el0` (ARM64) in ~6–8 ns.
* **Frequency Calibration:** One-time 5 ms calibration window during `ctrace_init()` determines clock frequency.
* **Fast Fixed-Point Conversion:** Nanoseconds are computed using a single 128-bit multiplication and 32-bit shift (`mult >> 32`) with zero runtime integer divisions.

### 4.7 Complex Structures, Wire Formats & Dynamic Counted Arrays
* **Hierarchical Struct Serialization:** `ctrace-scan` discovers struct hierarchies up to 4 levels deep, supporting passing structs by value or by pointer (`const Device *`, `&dev`, etc.) with safe `(null)` pointer dereferencing.
* **`CTRACE_COUNTED_BY(field)`:** Pointer fields (`T *`) and Flexible Array Members (`T []`) annotated with `CTRACE_COUNTED_BY(count_field)` are captured dynamically without copying unused buffer space.
* **Multi-Layer Nesting:** Dynamic arrays can be arbitrarily nested (e.g. a batch containing dynamic arrays of sensors, each containing dynamic arrays of float readings).
* **Out-of-Line Bindings:** `CTRACE_BIND_COUNT(type, array, count)` and `CTRACE_BIND_UNION(type, tag, value, member)` attach count and discriminator information to structs from headers that cannot be edited. The scanner records them as `annotate` attributes on dummy variables and applies them while resolving the struct.
* **Discriminated Unions:** A bound union is serialized as `uint32_t len` plus the active member selected by the sibling tag field (any `int64_t`-representable value, several values may map to one member). Each union in a struct is bound independently; anonymous unions and unions reached through struct pointers are handled. Unbound unions and unknown tag values fall back to the raw `sizeof(union)` bytes.
* **Fixed Arrays:** In-struct scalar arrays longer than 4 elements and arrays passed directly to `TRACE_EVENT` are captured as contiguous `fixed_array` blobs; shorter in-struct arrays are unrolled into `name_0 .. name_N` scalars. Arrays of structs longer than 4 elements are `counted_array` fields with a constant `array_len`, serialized element by element so that struct padding never reaches the wire. C11 anonymous struct members are spliced into their parent.
* **Skipped Fields:** `CTRACE_SKIP` on a field, or `CTRACE_BIND_SKIP(type, field)` out of line, removes the field from the schema and from every backend.
* **Decoder Output Order:** per-thread ring buffers are drained independently, so both decoders sort the decoded events by timestamp before printing.
* **Backends Support:**
  * `stdout`: Unrolls nested arrays into JSON-like formatting: `batch={num=2, sensors=[{id=1, readings=[1.5, 2.5]}]}`.
  * `bin` / `bin-zstd`: Serializes length prefix (`uint32_t`) followed by exact dynamic payload bytes in-place.
  * `ctf`: Encodes into CTF dynamically-sized arrays (`uint8_t payload[len]`).
  * `sqlite`: Binds dynamic arrays as exact-length `BLOB` fields (`sqlite3_bind_blob`).

### 4.8 Threading, Concurrency & Saturation Backpressure
* **Per-Thread SPSC Ring Buffers:** Each thread allocates a 256 KB memory buffer via `_Thread_local`. Tracing threads execute zero atomic Compare-And-Swap (`cmpxchg`) loops and zero cross-thread locks.
* **Saturation Policy:** When a thread outpaces the background writer, it executes a 100 µs micro-yield, followed by an immediate graceful drop if still full, guaranteeing that logging never blocks real-time application worker loops.
* **Real-Time Guard:** Background drainer thread automatically clamps to `SCHED_OTHER` if created from `SCHED_FIFO` / `SCHED_RR` callers.
* **Affinity & Tuning:** `ctrace_set_writer_affinity(cpu)` and `ctrace_set_writer_priority(nice)` allow binding background I/O to unisolated housekeeping cores.

---

## 5. CMake Integration Design

`ctrace` provides a clean CMake module (`ctrace.cmake`) exporting a single function: `target_enable_ctrace(<target>)`.

### 5.1 Directory Structure
```text
ctrace/
├── CMakeLists.txt
├── cmake/
│   └── ctrace.cmake          # Public CMake integration API
├── include/
│   └── ctrace/
│       └── trace.h           # Unified user macro header
├── runtime/
│   ├── rt_common.c
│   ├── rt_ringbuf.c          # Shared by bin, bin-zstd, and async sqlite
│   ├── rt_zstd.c
│   ├── rt_sqlite.c
│   └── rt_ctf.c             # CTF packet framing and TSDL metadata
└── tools/
    ├── ctrace-scan/          # C++ Clang LibTooling AST extractor
    └── ctrace-gen/           # Schema merger & C code generator
```

### 5.2 `cmake/ctrace.cmake` Implementation
```cmake
set(CTRACE_BACKEND "stdout" CACHE STRING "Trace backend: stdout | bin | bin-zstd | sqlite")
set_property(CACHE CTRACE_BACKEND PROPERTY STRINGS "stdout" "bin" "bin-zstd" "sqlite")

function(target_enable_ctrace TARGET_NAME)
    # 1. Require compile_commands.json for Clang LibTooling
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON PARENT_SCOPE)

    get_target_property(TARGET_SOURCES ${TARGET_NAME} SOURCES)
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_${TARGET_NAME}")
    file(MAKE_DIRECTORY "${GEN_DIR}")

    set(TRACE_SIDECARS "")

    # 2. Per-source AST scan step (incremental per .c file)
    foreach(SRC ${TARGET_SOURCES})
        if(SRC MATCHES "\\.c$")
            get_filename_component(SRC_ABS "${SRC}" ABSOLUTE)
            get_filename_component(SRC_NAME "${SRC}" NAME)
            set(SIDECAR "${GEN_DIR}/${SRC_NAME}.trace.json")

            add_custom_command(
                OUTPUT "${SIDECAR}"
                COMMAND $<TARGET_FILE:ctrace-scan>
                        -p "${CMAKE_BINARY_DIR}"
                        --extra-arg=-DCTRACE_AST_SCAN
                        --extra-arg=-I${CTRACE_INCLUDE_DIR}
                        -o "${SIDECAR}"
                        "${SRC_ABS}"
                DEPENDS "${SRC_ABS}" ctrace-scan "${CMAKE_BINARY_DIR}/compile_commands.json"
                COMMENT "[ctrace] Scanning AST for ${SRC_NAME}"
                VERBATIM
            )
            list(APPEND TRACE_SIDECARS "${SIDECAR}")
        endif()
    endforeach()

    # 3. Merge sidecars & generate backend-specific header
    set(GEN_HEADER "${GEN_DIR}/trace_generated.h")
    set(GEN_SCHEMA "${GEN_DIR}/trace_schema.json")

    add_custom_command(
        OUTPUT "${GEN_HEADER}" "${GEN_SCHEMA}"
        COMMAND $<TARGET_FILE:ctrace-gen>
                --backend=${CTRACE_BACKEND}
                --out-header="${GEN_HEADER}"
                --out-schema="${GEN_SCHEMA}"
                ${TRACE_SIDECARS}
        DEPENDS ${TRACE_SIDECARS} ctrace-gen
        COMMENT "[ctrace] Generating ${CTRACE_BACKEND} emitters for ${TARGET_NAME}"
        VERBATIM
    )

    # 4. Wire generated header & backend runtime library into target
    add_custom_target(${TARGET_NAME}_ctrace_codegen DEPENDS "${GEN_HEADER}")
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_ctrace_codegen)

    target_include_directories(${TARGET_NAME} PRIVATE "${CTRACE_INCLUDE_DIR}" "${GEN_DIR}")
    target_link_libraries(${TARGET_NAME} PRIVATE ctrace_rt_${CTRACE_BACKEND})
endfunction()
```

### 5.3 Consumer Usage (`CMakeLists.txt`)
```cmake
cmake_minimum_required(VERSION 3.20)
project(my_service C)

find_package(ctrace REQUIRED)

add_executable(my_service src/main.c src/net.c)
target_enable_ctrace(my_service)
```
Switching backends requires only a reconfigure/rebuild without touching C source files:
```bash
cmake -B build -DCTRACE_BACKEND=sqlite
cmake --build build
```

---

## 6. Error Handling & Edge Cases

1. **Signature Mismatch Across Translation Units:** If `src/a.c` calls `TRACE_EVENT(req, int_id)` and `src/b.c` calls `TRACE_EVENT(req, str_id)`, `ctrace-gen` aborts the build with a clear diagnostic showing both file paths and line numbers.
2. **Dangling String Pointers in Async Mode:** In `bin` / `bin-zstd` modes, strings are copied inline into the ring buffer immediately on the calling thread (`memcpy`), so stack-allocated buffers are safe. In `sqlite` mode, `SQLITE_TRANSIENT` is used unless `CTRACE_SQLITE_SYNC` is active.
3. **Header-Only / Inline Function Events:** If `TRACE_EVENT` is placed inside a `static inline` function in a shared header, `ctrace-scan` deduplicates identical call sites by `(event_name, canonical_file, line_number)` before emitting sidecars.
4. **Zero-Event Builds:** If a target links `ctrace` but contains zero `TRACE_EVENT` calls, `ctrace-gen` emits an empty valid `trace_generated.h` and stub `ctrace_init()` so builds succeed cleanly.
