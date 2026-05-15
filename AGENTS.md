# SCA — Agent Instructions

## Cursor Cloud specific instructions

### Services overview

This codebase produces two UEFI EFI binaries (`PlatformInit.efi`, `HypeDrain.efi`) via EDK2, plus a Windows-only installer (`sca-svc.exe`) via CMake+MSVC. Only the EFI build can run in this Linux environment; the installer requires Windows + MSVC 2022.

### Build (HV — the primary component)

EDK2 lives at `/workspace/edk2` (cloned from `edk2-stable202411`). To build:

```bash
cd /workspace/edk2
export WORKSPACE=/workspace/edk2
export EDK_TOOLS_PATH=/workspace/edk2/BaseTools
export PATH="/workspace/edk2/BaseTools/BinWrappers/PosixLike:$PATH"
export PACKAGES_PATH=/workspace/edk2
export GCC_BIN="$(dirname "$(which gcc)")/"
export PYTHON_COMMAND=python3
build -p SCAPkg/SCAPkg.dsc -a X64 -t GCC -b RELEASE
```

Outputs: `Build/SCAPkg/RELEASE_GCC/X64/PlatformInit.efi` and `HypeDrain.efi`.

### Gotcha: `-fstack-protector` in tools_def.txt

EDK2 202411's generated `Conf/tools_def.txt` has `-fstack-protector` in `GCC_ALL_CC_FLAGS`. With GCC 13+ this causes linker errors (`__stack_chk_guard` undefined) because `StackCheckLibNull` isn't force-linked into every module. The update script patches this to `-fno-stack-protector`. If you regenerate `Conf/tools_def.txt` (e.g. by deleting it and re-sourcing `edksetup.sh`), re-apply the patch:

```bash
sed -i 's/-fstack-protector/-fno-stack-protector/g' /workspace/edk2/Conf/tools_def.txt
```

### Lint / validation

```bash
python3 tools/check_log_decoder.py
```

This drift gate verifies every `HvLog`/`HvLogHex` code in `*.c` appears in `LOG_DECODER.txt` (and vice versa). It runs before every HV build in the production pipeline.

### Installer (Windows-only, cannot build here)

The `installer/` directory contains C++20 code targeting MSVC 2022 on Windows. It links `kernel32` and `advapi32`. See `installer/README.md` for modes and usage.

### SCAPkg symlink

The EDK2 build expects the source at `edk2/SCAPkg/`. The update script creates a symlink: `/workspace/edk2/SCAPkg -> /workspace`. This means all source files in the repo root are accessible as `SCAPkg/HypeEntry.c`, etc.
