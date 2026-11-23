# Standalone Plume host foundation

This is an independent Linux-only CMake project. It is not added to the native
game build. Its constructor is inert; `Backend::initialize()` is the only
startup path. It creates the Plume Vulkan interface, verifies one unique exact
device name before creation, then checks the returned name before making a
`DIRECT` queue.

Call `initialize()` only under separately owner-approved GPU coordination and
a card that explicitly authorizes Vulkan initialization. The current foundation
card permits only CPU-side compile/link and selection checks.

The default ignored vendor source path is
`dependencies/plume-clean-snapshot`, archived from parent commit
`d8ad43ca8cf54e9173619b5ded1ba8022f124f48`. The later parent snapshot contains
WWE13-only performance switches and fault-diagnostic externs and is not used.

**Do not link this module into the game or SDK yet.** The pinned Plume object
defines Volk/VMA and global `vk*` symbols that collide with the ReXGlue SDK.
Game linkage stays prohibited until a separate 2K14 SDK coexistence test proves
symbol isolation and ABI compatibility. This module does not apply ELF
localization or modify the SDK.

Import and build instructions, upstream comparison evidence, pinned source
revisions, and integrity hashes are recorded in
[decision 0002](../../docs/decisions/0002-plume-foundation.md).
The CPU-only checks do not initialize Vulkan, enumerate devices, or submit GPU
work.
