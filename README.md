<img width="3352" height="752" alt="The" src="https://github.com/user-attachments/assets/9edd72f1-a4c5-49da-ac38-6cd835661797" />

# Arvor Linux : When Atomicity Meets Mutability.
ARM Readme:https://github.com/NextFerret/arvor/blob/main/README-ARM.md

**Arvor Linux** is a Debian-based Linux distribution designed to provide **atomic system updates without relying on an immutable root filesystem**.

Unlike many modern atomic distributions, Arvor separates the concepts of **atomicity** and **immutability**. The project focuses on reliable, transactional upgrades while preserving a traditional, fully mutable Linux environment.

---

# Atomic ≠ Immutable

These terms are often used interchangeably, but they describe different concepts.

## Atomicity

**Atomicity** is the property of an operation that either:

- completes successfully; or
- does not happen at all.

No partially applied state is ever exposed.

Examples:

- Transactional database commits
- Transactional operating system updates

Atomicity prevents incomplete upgrades from leaving the system in an inconsistent state.

---

## Immutability

**Immutability** means an object or filesystem cannot be modified after it has been created.

Examples include:

- Read-only root filesystems
- OSTree deployments
- Container image layers

Immutability is one possible implementation strategy for atomic updates, but **it is not a requirement**.

---

## Arvor's Approach

Arvor provides atomic upgrades while maintaining a traditional mutable Linux system.

Instead of using technologies such as:

- OverlayFS
- OSTree
- Read-only root filesystems

Arvor relies on:

- LVM snapshots
- Transactional package operations
- Rollback support
- Native Debian compatibility

The result is a system that remains familiar to Linux administrators while providing recovery from failed upgrades.

---

# Core Components

| Component | Description |
|----------|-------------|
| **napt** | Transactional package manager compatible with APT workflows. |
| **nsm** | Snapshot Manager responsible for creating, managing and restoring system snapshots. |
| **nlc** | Lightweight chroot environment manager used internally by Arvor. |

---

# System Requirements

Minimum requirements:

- **CPU:** Any x86_64 processor from approximately the last 15 years
- **Memory:** 4 GB RAM
- **Storage:** 32 GB available disk space

Recommended:

- SSD
- 8 GB RAM or more

---

# Architecture

Arvor intentionally avoids technologies commonly used by immutable distributions.

| Technology | Used |
|-----------|------|
| OverlayFS | ❌ |
| OSTree | ❌ |
| Read-only root filesystem | ❌ |
| LVM Snapshots | ✅ |
| Mutable root filesystem | ✅ |
| Transactional updates | ✅ |

---

# Frequently Asked Questions

## Does Arvor use OverlayFS?

No.

Arvor intentionally avoids OverlayFS due to its long history of privilege escalation vulnerabilities and because it does not fit the project's design goals.

---

## Does Arvor use OSTree?

No.

Arvor implements its own transactional update model.

---

## Is the system immutable?

No.

Arvor is fully mutable while still supporting atomic upgrades and rollbacks.

---

## Is Arvor written entirely in Python?

No.

Earlier releases relied heavily on Python.

Beginning with **Arvor 2.1**, the **Project Xesta** initiative migrated most core utilities to **C and C++** to improve performance, reduce runtime dependencies and simplify distribution.

---

## What happened to NF-Tree?

**NF-Tree** was Arvor's original snapshot manager based on Btrfs.

During the development of Arvor 2.1, it was replaced by **NSM (NextFerret Snapshot Manager)** after a major redesign of the snapshot infrastructure.

NSM provides a cleaner architecture and serves as the foundation for future releases.

---

# License

**Software Cujo Código é Aberto (SCCA)**

Copyright © NextFerret

Licensed under the **Arvor License v1**.
