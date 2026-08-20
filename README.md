# VernisOS (Modular OS with AI Core)

> **Version 0.1.0-dev** — A bare-metal microkernel OS with an in-kernel Rust AI engine.
> Supports x86 and x86_64 from a single 44MB disk image.

## Quick Start

```bash
make prerequisites    # Check toolchain (i686-elf-gcc, x86_64-elf-gcc, nasm, cargo nightly, qemu)
make all               # Build everything → os.img
make run64              # Run in QEMU (x64)
```

📖 **[Getting Started Guide](GETTING_STARTED.md)** — full setup instructions
📖 **[CLAUDE.md](CLAUDE.md)** — build commands, architecture deep-dive, subsystem map (local reference; not tracked in this repo)

## Features

- **Dual-architecture**: x86 (32-bit) and x86_64 (64-bit) from one boot image, 3-stage bootloader with runtime CPUID arch detection
- **Microkernel**: round-robin preemptive scheduler, IPC (mailboxes + Unix sockets), dynamic module loader, sandboxing
- **In-kernel AI Engine** (Rust `no_std`): anomaly detection, scheduler auto-tuning, trust scoring, policy enforcement — 8 modules under `kernel/core/verniskernel/src/ai/`
- **Policy system**: YAML → compiled VPOL binary → kernel enforcement at boot
- **Storage stack**: VernisFS (native, sector-based) with NVMe > AHCI > ATA PIO auto-detection; read-only FAT32 and ext2 mounted alongside at `/mnt/fat32` and `/mnt/ext2`
- **Networking**: E1000 NIC driver, ARP/ICMP, full TCP (handshake, sliding-window send, go-back-N retransmit, out-of-order reassembly) and UDP (with DHCP client + DNS resolver), userland socket syscalls (`socket`/`connect`/`bind`/`listen`/`accept`)
- **Multiuser**: `/etc/shadow`-backed authentication (SHA-256), getty → login → shell chain, per-user home directories
- **Process model**: per-process address spaces, copy-on-write `fork`, blocking `waitpid`, ELF `execve`
- **GUI**: glassmorphism compositor with real backdrop blur, window manager with damage tracking, terminal widget — auto-resolution framebuffer, 240Hz render cadence
- **Security stack**: policy enforcement, sandboxing, SHA-256, audit logging
- **Multi-agent dev workflow**: a fixed subagent team (`pm`, `solution-architect`, `UX/UI`, `dev`, `tester`, `qa`, `security`, `optimizer`, `devops`, `doc-writer`, `system_analysis`) drives feature work, defined locally under `.claude/` (not tracked in this repo)

## Documentation

| Document | Description |
|----------|--------------|
| [GETTING_STARTED.md](GETTING_STARTED.md) | Prerequisites, build, first boot |
| [docs/OVERVIEW.md](docs/OVERVIEW.md) | High-level project overview |
| [docs/README.md](docs/README.md) | Docs index |
| [docs/STATUS.md](docs/STATUS.md) | Current implementation status by phase |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Development guidelines |
| [CHANGELOG.md](CHANGELOG.md) | Version history (all phases) |

## แนวความคิดในการพัฒนา

ระบบปฏิบัติการที่พัฒนาขึ้นนี้มีจุดมุ่งหมายเพื่อสร้าง **ระบบปฏิบัติการที่ยืดหยุ่นและรวดเร็ว** โดยการใช้ **สถาปัตยกรรม Microkernel** ซึ่งแบ่งออกเป็นหลายส่วนที่ทำงานแยกกันอย่างอิสระ เช่น Bootloader, Kernel, Modules, และ User Environment ซึ่งทั้งหมดสามารถใช้งานร่วมกันได้แบบ **modular** นอกจากนี้ ระบบยังฝัง **AI Engine** ไว้ใน **Core System** เพื่อช่วยตรวจสอบ ปรับแต่งระบบ และวิเคราะห์ข้อมูลต่าง ๆ เช่น crash log และ process anomalies โดยติดต่อกับ AI ผ่าน **CLI/Terminal** ตามสิทธิ์การเข้าถึงของผู้ใช้ พร้อมทั้งมี **GUI compositor** สำหรับสภาพแวดล้อมแบบหน้าต่างเต็มรูปแบบ

## จุดประสงค์ของโปรเจค

1. **สร้าง OS แบบ modular** ที่สามารถแยกแต่ละส่วนออกจากกัน (bootloader / kernel / modules / userland)
2. **รวม AI Engine เข้ากับ Core System** เพื่อเพิ่มความสามารถในการตรวจสอบและปรับปรุงการทำงานของระบบแบบ real-time
3. **ระบบที่ปลอดภัยและยืดหยุ่น** ด้วย policy enforcement, sandboxing และ multiuser authentication
4. **รองรับ storage และ network stack ที่ใช้งานได้จริง** — VernisFS, FAT32/ext2 (อ่านอย่างเดียว), TCP/UDP เต็มรูปแบบ
