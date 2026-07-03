# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
make prerequisites      # Check toolchain (i686-elf-gcc, x86_64-elf-gcc, nasm, cargo nightly, qemu)
make all                # Build everything → os.img (4MB disk image)
make build-x86          # x86 kernel only
make build-x64          # x64 kernel only
make rust               # Rebuild Rust static libs (both arches) — triggers cargo clean + full rebuild
make rust32             # Force-rebuild x86 Rust lib only
make rust64             # Force-rebuild x64 Rust lib only
make clean              # Remove build artifacts
```

> **Warning**: `make` without arguments does NOT build `all`. Pattern rules at the top of the Makefile
> make `make/kernel/arch/x86/syscall.o` the default goal. Always use `make all` explicitly.

## Run & Debug

```bash
make run32              # QEMU x86
make run64              # QEMU x64
make run64-ai           # QEMU x64 + AI bridge (start python3 ai/ai_listener.py --port 4444 first)
make debug32            # QEMU x86 + GDB stub :1234
make debug64            # QEMU x64 + GDB stub :1234
```

## Test

```bash
make test               # Integration tests (x64) via Python
make test-x86           # Integration tests (x86)
make test-all           # Both arches
python3 ai/tests/test_integration.py --arch x64 --img os.img
python3 ai/tests/test_cli_permission.py
```

## Policy & Filesystem

```bash
# Recompile policy after editing ai/config/policy.yaml:
python3 ai/tools/policy_compile.py ai/config/policy.yaml -o make/policy.bin

# Rebuild VernisFS after adding/changing user programs:
python3 ai/tools/mkfs_vernis.py -o make/vernisfs.bin \
    --vsh32 make/user/vsh32.elf --vsh64 make/user/vsh64.elf \
    --getty make/user/getty64.elf --login make/user/login64.elf
```

## Required Toolchain

- `i686-elf-gcc` / `x86_64-elf-gcc` — cross compilers (bare-metal ELF)
- `nasm` — assembler for boot stages and ASM stubs
- `cargo +nightly` — Rust nightly for `no_std` kernel (uses `-Zbuild-std`)
- `qemu-system-i386` / `qemu-system-x86_64` — emulator
- `python3` — policy compiler, mkfs tool, integration tests

---

## Architecture Overview

VernisOS is a bare-metal microkernel OS targeting x86 (i686) and x86_64 from a single 4MB `os.img`.

### Disk Image Layout

```
Sector 0      — Stage 1 bootloader (512B, real mode, CPUID arch detection)
Sectors 1–6   — Stage 2 (A20, GDT, routes to x86 or x64 path)
Sectors 7–12  — Stage 3 (PAE, PML4 paging, long mode entry) [boot/CISC/stage3.asm]
Sectors 13+   — x86 kernel binary
Sector 2048   — x64 kernel binary
Sector 4096   — Policy blob (VPOL binary compiled from ai/config/policy.yaml)
Sector 5120   — VernisFS image (/bin/vsh32, /bin/vsh64, /sbin/getty, /bin/login, etc.)
```

### Language Split

- **Assembly (NASM)**: `boot/x86/stage1.asm`, `boot/x86/stage2.asm`, `boot/CISC/stage3.asm`, interrupt/syscall stubs in `kernel/arch/`
- **C**: kernel arch entries (`kernel/arch/x86/kernel_x86.c`, `kernel/arch/x86_64/kernel_x64.c`), all subsystem `.c` files
- **Rust (`no_std`)**: `kernel/core/verniskernel/` → compiled to `lib/x86/libvernisos.a` and `lib/x86_64/libvernisos_x64.a`
- **Python**: AI tools (`ai/`), policy compiler, mkfs, integration tests

### Boot Flow

Stage 1 (MBR) runs CPUID and stores the arch flag at `0x7FF0`. Stage 2 reads that flag: if x86_64, it loads Stage 3 (`boot/CISC/stage3.asm`) from sectors 7–12 at `0x90000`, which sets up PML4 paging and jumps into long mode before loading the x64 kernel at `0x100000`. For i686, Stage 2 goes directly to protected mode and loads the x86 kernel.

### C ↔ Rust FFI

All Rust functions called from C are marked `#[no_mangle] extern "C"` in Rust and declared `extern` in C. The bridge goes in one direction: C calls Rust for scheduler, memory, AI engine, GUI, and console. `kernel/shims/rust_shims.c` provides the reverse direction — C symbols that Rust's runtime requires (`__rust_alloc`, `__rust_dealloc`, `rust_begin_unwind`, etc.).

Key Rust FFI groups declared in `kernel/arch/x86_64/kernel_x64.c`:
- `scheduler_new()`, `scheduler_create_process()`, `scheduler_schedule()`, `scheduler_set_quantum()` — Rust scheduler
- `fb_init()`, `console_putchar()`, `gui_init()`, `gui_main_loop_tick()` — framebuffer/GUI
- `ai_engine_init()`, `ai_engine_report_event()`, `ai_engine_get_action()` — AI engine FFI via `kernel/drivers/ai_engine.c`

### Rust Core Library (`kernel/core/verniskernel/src/`)

- `scheduler.rs` — round-robin preemptive scheduler, PCB, priority, signals
- `memory.rs` — buddy system heap allocator (`buddy_system_allocator` crate), 8MB heap
- `syscall.rs` — Rust-side syscall dispatch helpers
- `console.rs`, `framebuffer.rs` — VGA text + auto-resolution double-buffered GUI (bootloader scans the VBE mode list, picks the largest 32/24bpp LFB mode ≤ 2.1Mpx — 1920×1080×32 under QEMU)
- `ai/` — 8 modules: EventStore, AnomalyDetector, ProcessTracker, AlertDeduplicator, ResponseHandler, AutoTuner, PolicyEngine
- `gui/` — glassmorphism compositor (real backdrop blur: separable box blur + alpha tint + color-key content blit), window manager, terminal widget, cursor, widgets; render cadence follows the kernel timer (240Hz)
- GUI buffers are NOT heap-allocated (buddy allocator rounds to powers of two; the 8MB heap can't hold them). Fixed identity-mapped regions in `compositor.rs`: back buffer at 48MB, wallpaper cache at 56MB, glass-base cache at 64MB (8MB each; needs ≥72MB RAM). Do not enlarge `HEAP_SIZE`: BSS end + the frame-allocator pool must stay below the 0xF00000 kernel stack
- GUI perf model (QEMU is TCG on Apple Silicon — no hardware accel for x86 guests): typing restores the cached glass base (zero blur per keystroke); layout changes go through damage tracking in the window manager (move/focus/close accumulate rects; only damaged regions are restored/recomposed/presented — never the full frame); dragging renders tint-only glass at ~60fps and re-frosts on drop. Telemetry: `[gui] slow compose: ticks=N path=P` on serial above ~33ms (path 1=layout-partial, 2=full, 3=typing). Avoid per-pixel divides and per-pixel helper calls in render loops — use the 32bpp fast paths as templates
- `kernel/shims/rust_shims.c` memcpy/memset use rep movsq/stosq (movsl on i686), NOT byte loops: TCG emulates string ops per iteration, so a byte-loop memcpy caps all compositor restores and framebuffer presents at ~20MB/s and makes the whole GUI feel broken. Don't "simplify" them back
- CLI `winmove <dx> <dy>` moves the focused GUI window — use it to exercise the damage/compose path deterministically (synthetic PS/2 drags via the QEMU monitor lose packets)
- Scheduler quanta matter for GUI latency: round-robin has no blocking, so hlt-loop/poll-loop tasks (worker, vsh) get tiny quanta (2-4 ticks). Giving a busy-poll task a 24-tick quantum steals a third of the compositor's wall time
- `module_registry.rs` — kernel module registry

Rust build: `cargo +nightly build -Zbuild-std=core,alloc -Zjson-target-spec --target i386.json` (x86); x64 uses `x86_64-unknown-none` with `RUSTFLAGS="-C no-redzone=yes"`. Only run `make rust` when `kernel/core/verniskernel/src/` changes — it runs `cargo clean` first.

### Kernel Subsystems (C, under `kernel/`)

| Directory | Key files | Notes |
|-----------|-----------|-------|
| `kernel/arch/x86/` | `kernel_x86.c`, `interrupts.asm`, `syscall.asm` | x86 entry, IDT, int 0x80 |
| `kernel/arch/x86_64/` | `kernel_x64.c`, `interrupts.asm`, `syscall.asm` | x64 entry, SYSCALL/SYSRET, E1000 NIC driver |
| `kernel/drivers/` | `ai_bridge.c`, `ai_engine.c`, `acpi.c` | COM2↔Python AI bridge, Rust AI FFI wrappers |
| `kernel/fs/` | `vfs.c`, `vernisfs.c`, `bcache.c` | VFS layer, native FS, LRU block cache |
| `kernel/fs/` | `fat32.c`, `ext2.c`, `ntfs.c` | **Stubs only** — 3-line files, not implemented |
| `kernel/security/` | `policy_loader.c`, `policy_enforce.c`, `sandbox.c`, `userdb.c`, `sha256.c`, `auditlog.c` | Full security stack |
| `kernel/net/` | `tcp.c` | Handshake/data/close + RFC 793 checksum work (Phase 49); no sliding window or data retransmit |
| `kernel/ipc/` | `ipc.c` | Mailbox + Unix socket layer |
| `kernel/shell/` | `cli.c` | ~30 built-in commands, pipe support |
| `kernel/module/` | `module.c`, `dylib.c` | Dynamic module loading |
| `kernel/shims/` | `rust_shims.c` | C stubs for Rust runtime symbols |

All `include/*.h` headers are shared between x86 and x64 builds (`-I include`). Headers `memory_base.h`, `scheduler_base.h`, `syscall_base.h` are FFI-only with no `.c` implementation.

### VFS / Storage Stack

```
CLI / Syscall
    ↓
kfs_*() — VFS abstraction (kernel/fs/vfs.c)
    ↓
bcache (LRU write-back, 64 × 512B blocks)
    ↓
Backend: NVMe > AHCI > ATA PIO (auto-detected at boot)
    ↓
VernisFS on disk (sector-based, 32 files max)
```

### Network / E1000

The Intel E1000 (82540EM) driver lives in each arch's kernel file (`kernel/arch/x86_64/kernel_x64.c` and `kernel/arch/x86/kernel_x86.c`) as static functions (`e1000_send()`, `e1000_recv()`). The timer IRQ (240Hz) calls `net_rx_poll()`, which drains the RX ring into `net_dispatch_frame()` — ARP, ICMP echo replies (ping waits on `g_icmp_echo_seq`), and TCP (`tcp_receive_packet()`).

TCP (`kernel/net/tcp.c`) is arch-agnostic: the kernel registers `net_tcp_ip_output()` via `tcp_set_output()` at boot. That transmit path is non-blocking — on ARP cache miss it drops the segment and fires an ARP request; handshake retransmission (`tcp_tick()`, also timer-IRQ) covers the loss. Never call the blocking `net_arp_resolve()` from IRQ context. TCP covers handshake, data (`tcp_send`/`tcp_recv`, 1KB rx buffer per socket), teardown, and RFC 793 checksum; still missing: sliding window, data retransmission, out-of-order reassembly.

UDP (`kernel/net/udp.c`, same arch-agnostic pattern) provides 8 sockets with `udp_bind/udp_sendto/udp_recvfrom`; the same file hosts the DHCP client (full DORA; `dhcp` CLI applies the lease via `kernel_net_apply_config()`) and DNS A-record resolver (`nslookup` CLI; server defaults to slirp's 10.0.2.3). UDP's IP transmit supports src 0.0.0.0 and 255.255.255.255 broadcast for DHCP. CLI: `udpbind/udpsend/udprecv/udpclose`, `dhcp`, `nslookup`.

Serial console: the timer IRQ also polls COM1 RX into the kernel CLI keyboard buffer, so `-serial stdio` (plus `-vga none` to force VGA-text CLI mode instead of the GUI) drives the shell — this is how the integration tests interact with the OS. CLI test commands: `tcphandshake connect <ip> <port>`, `tcpsend`, `tcprecv`, `tcpstat`, `tcpclose`.

### AI Integration

- **In-kernel** (primary): Rust AI engine in `kernel/core/verniskernel/src/ai/`. Called from C via `kernel/drivers/ai_engine.c` FFI wrappers. Handles anomaly detection, scheduler quantum auto-tuning, trust scoring, policy enforcement.
- **External** (dev/test): `ai/ai_listener.py` on TCP 4444; kernel sends events over COM2 serial (`kernel/drivers/ai_bridge.c`). Used with `make run64-ai`.
- **Policy flow**: edit `ai/config/policy.yaml` → `python3 ai/tools/policy_compile.py` → VPOL binary at sector 4096 → `kernel/security/policy_loader.c` parses at boot.

### User Space

Programs in `userlib/` use bare-metal cross-compilers (no libc). Pattern: `crt0_{x86,x64}.asm` provides `_start` → calls `main()` → `SYS_EXIT`. Syscall wrappers are in `userlib/syscall.h` (int 0x80 for both arches currently).

Load addresses: code at `0x10000000`, mmap region at `0x20000000` (x64) / `0x30000000` (x86).

Each user process has its own address space (`TaskSlot.pml4` on x64, `.pd` on x86). `paging_create_address_space` clones the 0–1GB paging chain — kernel identity + framebuffer mappings are shared by value, the user range starts empty. CR3 is switched on context switch (timer), execve, exit and kill. `elf_exec`/`execve` build into a fresh space then free the old one *after* switching CR3 (never free what CR3 still points at); `fork` deep-copies every mapped user page. Frames are reclaimed via `frame_free` into a free-list, so exec/exit/kill cycles don't exhaust the pool. `/sbin/init{32,64}` (userlib/init.c) boots as the first user process; a kernel "kinit" task is the fallback if init is absent.

To add a new user program:
1. Create `userlib/myprog.c` using `syscall.h` + `libc.h`
2. Add compile + link rules to `Makefile` following the `getty`/`login` pattern
3. Add `--myprog` argument to `mkfs_vernis.py` and pass the ELF in the `$(VFS_BIN)` Makefile rule

VernisFS holds max 32 files. Current image uses 10.

### Build Outputs

All intermediate files go under `make/` (not committed):
- `make/boot/x86/stage{1,2}.bin`, `make/boot/CISC/stage3.bin`
- `make/kernel/arch/x86/kernel_x86.{o,elf,bin}`
- `make/kernel/arch/x86_64/kernel_x64.{o,elf,bin}`
- `make/user/vsh{32,64}.elf`, `getty64.elf`, `login64.elf`
- `make/policy.bin`, `make/vernisfs.bin`
- `lib/x86/libvernisos.a`, `lib/x86_64/libvernisos_x64.a`

### Known Incomplete Subsystems

| Subsystem | Location | Status |
|-----------|----------|--------|
| TCP extras | `kernel/net/tcp.c` | Core works (handshake/data/close, checksum); no sliding window, data retransmit, OOO reassembly |
| Socket fd model | — | UDP/TCP not exposed as file descriptors yet (Phase 52) |
| Copy-on-write fork | `kernel/arch/*/kernel_*.c` | Per-process address spaces exist (each task has its own PML4/page-dir, CR3 switched on context switch); fork does a full deep page copy, not CoW. Frames come from a free-list on top of the bump allocator |
| FAT32 driver | `kernel/fs/fat32.c` | 3-line stub — ignore STATUS.md claim |
| ext2 driver | `kernel/fs/ext2.c` | 3-line stub |
| NTFS driver | `kernel/fs/ntfs.c` | 3-line stub |
| Makefile default goal | `Makefile:1` | First target is `syscall.o` not `all` — always run `make all` |
