# SCA/tools — script index

Orchestration layer for the SCA HV + installer + dump pipeline. Every script here drives PC1 from the share over SSH. No script runs on PC1 standalone.

## Quick start

```bash
./tools/sca.sh                  # full iteration: HV build + deploy + installer build + deploy
./tools/sca.sh hv               # HV build + deploy only
./tools/sca.sh installer        # build SCAhost.exe + sign + push to PC1
./tools/sca.sh build            # HV build only (no deploy)
./tools/run_installer.sh        # run SCAhost.exe on PC1 (post-HV-boot, post-game-launch)
./tools/drain.sh                # live drain (HV stays up)
./tools/drain.sh --reboot       # post-BSOD/wedge drain only
```

## Script reference

| Script | Role | Runs on | PC1 artifact |
|---|---|---|---|
| [sca.sh](sca.sh) | Orchestrator. Modes: `hv` / `installer` / `build` / `all` (default). | share | n/a — calls others |
| [build_sca_hv.sh](build_sca_hv.sh) | In-tree EDK2 build (`PACKAGES_PATH=$EDK2:SCA`, no rsync). Runs `check_log_decoder.py` drift gate first, then GCC RELEASE X64 build of `PlatformInit.efi` + `HypeDrain.efi`. | share | (none — artifacts stay in `Tools/EDK2/Build/SCAPkg/`) |
| [deploy_sca_hv.sh](deploy_sca_hv.sh) | SCP `PlatformInit.efi` + `HypeDrain.efi` → PC1 HYPEBOOT USB. SHA256 verify. | share | `HYPEBOOT:\EFI\Boot\PlatformInit.efi`, `HYPEBOOT:\HypeDrain.efi` |
| [build_deploy_installer.sh](build_deploy_installer.sh) | Tarball `SCA/installer/`, push to PC1, run `build_installer_local.bat` over SSH (MSVC 2022 BuildTools), pull binary, sign with `PEX/tools/sign/svc-codesign.pfx`, push back. Pushes both PS1 helpers too. | share | `C:\SCA\SCAhost.exe`, `get_target_peb.ps1`, `wait_target_peb.ps1` |
| [build_installer_local.bat](build_installer_local.bat) | MSVC vcvars + cmake configure + cmake build. Synced to `C:\Tools\` by the deploy script. | PC1 only | `C:\SCA\installer\build\Release\SCAhost.exe` (intermediate) |
| [run_installer.sh](run_installer.sh) | SSH-run `SCAhost.exe` on PC1 in default install mode. Default `--rva 0x26B87D` (cmd-list wrapper Close). Kills stale instances first. Auto-grabs PEB; `WAIT_TARGET_SEC=300` enables poll-for-Apex pre-launch flow. | share | runs install — patches `.text` at chosen RVA |
| [live_memdump.sh](live_memdump.sh) | SSH-run `SCAhost.exe --live-memdump C:\SCA`. Mailbox + Phase-4 CR3 + `BulkVirtRead8` only — never touches `HOOK_INSTALL_DRAW`. Wrapped by `UNICORN_DUMPER/redump.sh`. | share | writes `C:\SCA\r5apex_live_{text,rdata,data}.bin` |
| [drain.sh](drain.sh) | Pull 4 MB HV log ring. Live mode = `SCAhost.exe --drain` (HV stays up). `--reboot` mode drops `D:\drain.flag` + `shutdown /r` so `HypeDrain.efi` runs next boot. SCPs back to `SCA/drains/`, decodes 3-char codes against `LOG_DECODER.txt`, rotates oldest. | share | live: `C:\SCA\hypedbg-live-*.bin`. reboot: `D:\hypedbg-*.bin` |
| [get_target_peb.ps1](get_target_peb.ps1) | PowerShell: get r5apex_dx12 PEB VA. | PC1 | n/a |
| [wait_target_peb.ps1](wait_target_peb.ps1) | PowerShell: poll until r5apex_dx12 is up + stable, then return PEB VA. | PC1 | n/a |
| [check_log_decoder.py](check_log_decoder.py) | Drift gate. Walks every `HvLog(...)` / `HvLogHex(...)` call site in `SCA/*.c`, flags codes missing from `LOG_DECODER.txt` and orphan entries. Wired into `build_sca_hv.sh` before EDK2 build. | share | n/a |
| [startup.nsh](startup.nsh) | UEFI Shell script on HYPEBOOT USB — drain-flag check, then loads PlatformInit.efi, then chains bootmgfw.efi. | HYPEBOOT USB | n/a |

## `SCAhost.exe` modes (what `run_installer.sh` / `live_memdump.sh` / `drain.sh` actually invoke — replaces the old `sca-svc.exe` name)

Authoritative table: see [../installer/README.md](../installer/README.md). One sentence each:

| Mode | Trigger | Stays up? |
|---|---|---|
| install (default) | `--rva 0x…` | no |
| `--live-memdump <dir>` | explicit | minutes |
| `--drain` | explicit | seconds |
| `--reconfig` | `--glow-*` flags | no |
| `--drift-check` | explicit | no |
| `--dump-teams` | explicit | no |
| `--peek-ent <VA>` | explicit | no |
| `--peek-va <VA>` | explicit | no |
| `--bucket-write` | explicit | no |
| `--hid-set` | explicit | no |
| `--diag-m0f` | explicit | no |

## Troubleshooting

### "I get CTD when the installer runs"

First, verify these in order:

1. **Stale `SCAhost.exe` on PC1.** Source has `--live-memdump` but the deployed binary doesn't. `live_memdump.sh` passes the flag; the older binary ignores it, falls through to default install, and patches with `rva=0` (= overwrites PE header). **Check:** `findstr /C:LIVE_MEMDUMP_OK C:\SCA\SCAhost.exe` should hit. If empty, `sca.sh installer` to rebuild.
2. **Wrong `--rva` for current Apex build.** `run_installer.sh` default is `0x26B87D` — patches drift across Apex patches. Use `SCAhost.exe --drift-check --module-base <VA>` first to confirm pins.
3. **HV not booted.** apphost/pexsvc/ping-test launched without a fresh HV will CTD on the VMMCALL handshake. F11 → HYPEBOOT USB before any installer run.

### "live_memdump hangs / writes 0-byte bins"

Stale binary on PC1 (above) — `--live-memdump` was silently dropped. Rebuild + redeploy via `sca.sh installer`.

## Rules

- **One binary in `C:\SCA\SCAhost.exe`.** No `SCAhost.exe.bak`, no `installer\build\Release\SCAhost.exe`. Stale dupes get picked up and CTD Apex.
- Pre-deploy backup of `PlatformInit.efi` is enforced by `war_guard.py`. Obey mandatory backup workflow.
- Always verify `.efi` is non-zero after SCP — 0-byte loads silently and boots Windows without HV.
