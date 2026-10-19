# Cortez Kernel Embedded C Compiler (`third_party/tinycc`)

## Overview

This directory provides the freestanding, in-kernel implementation of the `libtcc` API (`libtcc.h` / `libtcc.cpp`) tailored specifically for the x86_64 Cortez Kernel (`-ffreestanding -mcmodel=kernel`).

## Cortez Kernel Adaptations

1. **Direct VFS Integration (`tcc_add_file` & `#include`)**:
   - Reads source files and `#include` headers directly through `cortez::fs::VfsManager::instance()` from `/ext/include` and the active working directory.
   - Includes built-in fallback system headers (`<cortez.h>`, `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<string.h>`, `<stdlib.h>`, `<stdio.h>`) so standalone files compile even if `/ext/include` is not present.
2. **Higher-Half Executable Relocation (`tcc_relocate`)**:
   - Two-phase relocation (`tcc_relocate(s, NULL)` queries total byte size; `tcc_relocate(s, mem)` relocates `.text`, `.rodata`, `.data`, and `.bss` into the caller-supplied `ModuleArea` buffer at `0xFFFFFFFFC0000000+`).
   - No `mmap` or `mprotect` syscalls are used; pages are mapped RWX by `cortez::memory::ModuleArea`.
3. **Exported Symbol Introspection**:
   - Adds `tcc_get_exported_symbol_count`, `tcc_get_exported_symbol_name`, and `tcc_get_exported_symbol_addr` so `ModuleManager` and `IdtManager` can resolve faulting `RIP` addresses back to `module_name:function+0xoffset` during CPU exception containment.
