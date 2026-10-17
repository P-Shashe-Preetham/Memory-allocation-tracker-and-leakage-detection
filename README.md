# MemWatch — Linux Memory Monitor and Leak Detector in C

**MemWatch** is a native Linux command-line utility implemented in C11 that monitors physical and swap memory usage via `/proc`, tracks per-process Resident Set Size (RSS) and Virtual Memory Size (VSZ), flags processes exhibiting sustained suspicious memory growth, and integrates with **Valgrind Memcheck** and **libxml2** for allocation-level memory leak diagnostics.

## 1. Architecture

```text
memwatch/
├── include/
│   ├── memwatch.h          # Core status codes, CLI options, and utility declarations
│   ├── proc_reader.h       # /proc/meminfo and /proc/[PID] data structures & parsers
│   ├── monitor.h           # Ring-buffer sample history, PID reuse check & growth heuristic
│   ├── analyzer.h          # Valgrind Memcheck runner & XML parser declarations
│   ├── report.h            # ANSI terminal table & saved report formatters
│   └── compat/libxml/      # Fallback ABI headers when libxml2-dev headers are absent
├── src/
│   ├── main.c              # Entry point and command dispatch
│   ├── cli.c               # Robust command-line parser with '--' target separator
│   ├── proc_reader.c       # /proc/meminfo, /proc/[PID]/status, stat, smaps_rollup readers
│   ├── system_memory.c     # 'system' and 'top' command implementations
│   ├── monitor.c           # Live process monitor, signal handling & growth detection
│   ├── leak_analyzer.c     # Safe fork()/execvp() Valgrind integration & report saving
│   ├── valgrind_parser.c   # libxml2 DOM parser for Valgrind Protocol 4 XML reports
│   ├── report.c            # Terminal table rendering, color control & report pruning
│   └── utils.c             # Safe string/number parsing, byte formatting & PATH lookup
├── tests/
│   ├── test_proc_reader.c          # Unit tests for procfs parsing & disappearing PIDs
│   ├── test_memory_calculations.c  # Unit tests for unit conversion, % math & ranking
│   ├── test_growth_detection.c     # Unit tests for growth heuristics & PID reuse reset
│   ├── test_report_parser.c        # Unit tests for XML parsing, malformed XML & fork/exec
│   ├── leak_target.c               # Standalone test binary (--clean and --leak modes)
│   └── mock_valgrind.c             # Deterministic Valgrind XML harness for offline tests
├── reports/
│   └── latest.xml          # Sample Valgrind Protocol 4 XML report
├── Makefile
├── README.md
└── .gitignore
```

## 2. Process Memory Monitoring vs. Allocation-Level Leak Detection

MemWatch separates two fundamentally distinct diagnostic techniques:

- **Process Memory Monitoring (`memwatch system`, `memwatch top`, `memwatch monitor`)**: Inspects kernel accounting structures in `/proc/[PID]/status` (`VmRSS`, `VmSize`) and `/proc/[PID]/smaps_rollup` (`Pss`, `Private_Dirty`) for already-running processes. A generic process monitor **cannot** prove that an individual `malloc()` block was leaked merely by observing RSS, because RSS also increases due to legitimate page caching, heap fragmentation, workload changes, or allocator arena retention (`brk`/`mmap` pools not yet returned to the kernel). Consequently, MemWatch labels upward RSS trends strictly as **suspected memory growth**.
- **Deep Allocation-Level Leak Detection (`memwatch analyze`, `memwatch report`)**: Launches a target executable from start to finish under **Valgrind Memcheck** (`--tool=memcheck --leak-check=full --show-leak-kinds=all --track-origins=yes --xml=yes`), tracking every heap allocation (`malloc`, `calloc`, `realloc`, `new`) and deallocation (`free`, `delete`) to classify unfreed blocks at exit with exact call stacks.

## 3. Data Collection from `procfs` and Memory Metrics

### System Memory (`/proc/meminfo`)
- **Total Physical Memory (`MemTotal`)**: Total usable physical RAM visible to the Linux kernel (excluding a small reserved binary/kernel region).
- **Available Memory (`MemAvailable`)**: The kernel's estimate of how much memory is available for starting new applications without swapping. Unlike `MemFree`, `MemAvailable` accounts for reclaimable page cache (`Cached`), buffer cache (`Buffers`), and slab reclaimable caches (`SReclaimable`) minus shared memory (`Shmem`). If `MemAvailable` is absent on older kernels, MemWatch computes `MemFree + Buffers + Cached + SReclaimable - Shmem`.
- **Used Memory (`MemTotal - MemAvailable`)**: Memory actively in use that cannot be immediately reclaimed without swapping. Computing `Used = MemTotal - MemFree` would inaccurately count reclaimable disk cache as consumed memory.
- **Swap Usage (`SwapTotal - SwapFree`)**: Reported whenever `SwapTotal > 0`, along with swap utilization percentage.

### Per-Process Metrics (`/proc/[PID]/status`, `/proc/[PID]/stat`, `/proc/[PID]/smaps_rollup`)
- **Resident Set Size (`VmRSS`)**: Portion of a process's memory currently resident in physical RAM (including shared library pages).
- **Virtual Memory Size (`VmSize`)**: Total virtual address space mapped by the process (including unbacked reservations, memory-mapped files, and swapped pages).
- **Proportional Set Size (`Pss` from `smaps_rollup`)**: Collected when accessible; divides shared library pages proportionally across sharing processes.
- **Missing Memory Values**: Kernel threads (such as `kthreadd` or `kworker/*`) and zombie processes do not expose `VmRSS` or `VmSize` in `/proc/[PID]/status`. MemWatch tracks validity flags (`rss_valid`, `vsz_valid`) and displays `N/A` rather than misinterpreting missing fields as `0 B`.

## 4. Sustained Memory Growth Detection Heuristic

For each monitored process, `monitor.c` maintains a bounded circular buffer (`MW_HISTORY_CAPACITY = 64` samples) up to `MW_MAX_TRACKED_PROCS = 2048` processes.

A process is flagged for **suspected memory growth** when all of the following hold:
1. **Minimum Sample Window**: At least `--min-samples` (default `4`) valid RSS measurements have been collected.
2. **Sustained Upward Trend**: Either at least `min_samples - 2` consecutive increases occur at the tail of the sample window, or the entire window is monotonically non-decreasing with multiple strict increases.
3. **Absolute Threshold**: Net RSS growth (`current_rss - initial_rss`) meets or exceeds `--threshold-kb` / `--threshold-bytes` (default `64 KiB` / `65,536 bytes`).
4. **Percentage Threshold**: Relative RSS growth meets or exceeds `--threshold-pct` (default `5.0%`).
5. **Minimum Duration**: Elapsed monitoring time meets or exceeds `--min-duration` (default `0` seconds).

### PID Reuse and Disappearance Protection
Linux recycles numeric PIDs after processes exit. MemWatch reads field 22 (`starttime` in clock ticks since boot) from `/proc/[PID]/stat` alongside the process name. If a PID disappears during a scan (`mw_tracker_prune_disappeared`) or reappears with a different `starttime_ticks`, its history slot is immediately reset so samples from two distinct processes never merge.

## 5. Valgrind Memcheck Integration and Leak Categories

When running `memwatch analyze -- ./program arg1 arg2`, MemWatch:
1. Validates the target executable and checks `PATH` for `valgrind` (or `--valgrind-bin` / `MEMWATCH_VALGRIND_BIN`).
2. Launches Valgrind directly via `fork()` and `execvp()` using an argument array (no `system()` or shell interpolation, preventing command injection).
3. Writes XML Protocol 4 output to `<output-dir>/valgrind_<timestamp>_<pid>.xml` and copies it to `<output-dir>/latest.xml`.
4. Preserves and parses the XML report even when the target program exits with a non-zero exit code.
5. Uses `libxml2` to parse each `<error>` node and classify unfreed blocks without double-counting:
   - **Definitely lost (`Leak_DefinitelyLost`)**: No pointer to the allocated block exists anywhere in memory; confirmed memory leak.
   - **Indirectly lost (`Leak_IndirectlyLost`)**: The block is only reachable through pointers inside another leaked block (e.g., child nodes of a leaked struct or tree).
   - **Possibly lost (`Leak_PossiblyLost`)**: Only interior pointers (pointing into the middle of the block rather than its start address) were found.
   - **Still reachable (`Leak_StillReachable`)**: Valid pointers to the start of the block still existed at program exit (e.g., global caches or singletons not freed before `exit()`).
   - **Suppressed (`Leak_Suppressed`)**: Ignored via a Valgrind suppression rule.

Compiling the target binary with DWARF debug symbols (`gcc -g`) enables Valgrind and MemWatch to resolve exact source files and line numbers (`file:line`) in stack traces.

## 6. Installation and Usage (Ubuntu Linux & Ubuntu WSL2)

### Prerequisites Installation
```bash
sudo apt update
sudo apt install build-essential gcc make pkg-config libxml2-dev valgrind
```

### Building and Testing
```bash
# Build memwatch and the standalone leak_target binary
make

# Build with debug symbols (-g -O0)
make debug

# Compile and execute the deterministic C unit test suite
make test

# Compile and run the unit test suite with AddressSanitizer and UndefinedBehaviorSanitizer
make sanitize

# Install to /usr/local/bin (or custom PREFIX=/opt/memwatch)
sudo make install

# Clean all build artifacts and temporary reports
make clean
```

### Command Reference & Examples
```bash
# 1. System memory & swap overview + top 10 processes
./memwatch system

# 2. Top processes ranked by resident memory (RSS)
./memwatch top --top 15

# 3. Live process monitor (refreshes in place on TTY; Ctrl+C to exit cleanly)
./memwatch monitor

# 4. Monitor a specific PID every 1 second for up to 60 seconds
./memwatch monitor --pid 1234 --interval 1 --duration 60

# 5. Deep allocation leak analysis with Valgrind Memcheck
./memwatch analyze -- ./tests/leak_target --leak

# 6. Parse and summarize a saved Valgrind XML report
./memwatch report reports/latest.xml

# 7. Display CLI help
./memwatch --help
```

## 7. Example Output

```text
MEMWATCH — MEMORY LEAK ANALYSIS

Target: ./tests/leak_target --leak
Analyzed PID: 18420

Definitely lost:        2,048 bytes  (1 blocks)
Indirectly lost:          512 bytes  (1 blocks)
Possibly lost:              0 bytes  (0 blocks)
Still reachable:        4,096 bytes  (1 blocks)
Suppressed:                 0 bytes  (0 blocks)
Total unfreed:          6,656 bytes  (aggregated without double-counting)

Allocation stack traces: Available where reported
Debug symbols note:      DWARF debug symbols detected (source file & line numbers resolved).

Detailed Findings (3 record(s)):

  [1] Definitely lost (Leak_DefinitelyLost)
      Summary: 2,048 bytes in 1 blocks are definitely lost in loss record 2 of 3
      Leaked:  2,048 bytes in 1 block(s)
        #0 0x4848899 in malloc (/usr/libexec/valgrind/vgpreload_memcheck-amd64-linux.so)
        #1 0x1091E2 at create_intentional_leak (leak_target.c:39)
        #2 0x10927A at main (leak_target.c:61)
```

## 8. Permissions, Troubleshooting, and Known Limitations

- **Permissions**: Ordinary system and per-process monitoring (`/proc/meminfo`, `/proc/[PID]/status`, `/proc/[PID]/stat`) requires **no root privileges**. However, reading `/proc/[PID]/smaps_rollup` for processes owned by other users or root requires `CAP_SYS_PTRACE` or `sudo` depending on `kernel.yama.ptrace_scope`. MemWatch counts and reports permission-restricted processes without crashing.
- **Ubuntu WSL2 Limitations**: Inside WSL2, `/proc/meminfo` and `/proc/[PID]` reflect the Linux utility VM's memory allocation and Linux processes—not the Windows host's global task list or Windows native processes. Physical RAM visible in WSL2 is bounded by the `.wslconfig` `memory=` limit.
- **Language Runtime Limitations**: Valgrind Memcheck is designed for native C, C++, and Rust binaries that use standard system allocators (`malloc`/`free`). Managed runtimes (Python, JVM, Node.js, Go) use custom arena allocators and garbage collectors that obscure individual object lifetimes from Valgrind unless runtime-specific flags or debug builds are used.
