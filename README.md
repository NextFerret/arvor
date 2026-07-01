<img width="3352" height="752" alt="The" src="https://github.com/user-attachments/assets/9edd72f1-a4c5-49da-ac38-6cd835661797" />

# Arvor Linux : When Atomicity Meets Mutability.
ARM Readme:https://github.com/NextFerret/arvor/blob/main/README-ARM.md

## We Moved to XFS and LVM in the version 4.1.

## Arvor Linux 4.1 Will recive an ARM Version called Armvor.
Arvor Linux is an Debian-Based Atomic&Mutable Solution.

You may say "IT IS IMPOSSIBLE,IMMUTABLE = ATOMIC"

Wrong,lets start **The ✨ Annoying ✨ and ✨ Useless ✨ Dictionary Moment**



atomicity (noun)

Pronunciation: /ˌætəˈmɪsəti/

Meaning:

(computing) The property of an operation or transaction being completed entirely or not at all, with no intermediate state visible.
The quality of being atomic; indivisibility.

Examples:

The database guarantees atomicity for all transactions.
Atomicity helps prevent data corruption during system updates.

Related words:

atomic (adjective)
atomically (adverb)

Plural: atomicities (rare)

immutability (noun)

Pronunciation: /ɪˌmjuːtəˈbɪləti/

Meaning:

The quality of being unable to be changed.
(computing) The property of an object, file, or system state that cannot be modified after it is created.

Examples:

Immutability is a key feature of many functional programming languages.
The operating system uses immutability to protect the user from itself.

Related words:

immutable (adjective)
immutably (adverb)

Plural: immutabilities (rare)

...SEE?

its not the same thing.
anywaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaay.

# Core Programs:

`nsm` : the snapshot manager of Arvor

`napt` : the atomic version of apt

`nlc` : chroot manager ← js added because i need a way more simplier to mount the chroot of this distro :3

# Minimum Requirements

32 GB of SSD/HDD

Any Modern CPU (15+ years)

4 GB of RAM.

# Software Cujo Código é Aberto (SCCA) by NextFerret by The Arvor License v1.
# FAQ:

### Does This Distro Uses OverlayFS?
No. In Fact we Actually Abomine using OverlayFS because of the historical LPEs Problem.

### Does Arvor uses 100% python on their codebase?
No. By the Release 2.1 We made Project Xesta,that turned the tools into C or C++

### Does Arvor Uses ostree?
No.

### What is nf-tree that google said? i did not see any nf-tree here!
nf-tree was our snapshot manager based on BTRFS. We changed it to nsm in 2.1 because nf-tree had a patched-up structure and code.
