# rl-telemetry-driver

A small, read-only Windows kernel driver that lets a user-mode program read
another process's memory, built for a Rocket League telemetry/overlay tool.

Easy Anti-Cheat strips `PROCESS_VM_READ` rights from handles opened on the game,
so `ReadProcessMemory` returns `ERROR_ACCESS_DENIED`. A driver that resolves the
target `PEPROCESS` with `PsLookupProcessByProcessId` is not affected by that
handle filtering, because it never uses the caller's handle.

## API

Device `\\.\RltRdDev`, three buffered IOCTLs:

| IOCTL | In | Out | Meaning |
|-------|----|-----|---------|
| `0x00222000` | ? | `u32` magic | liveness / version |
| `0x00222004` | `u32 pid` | `u64` image base | module base of the process |
| `0x00222008` | `u32 pid, u32 len, u64 addr` | `len` bytes | read memory |

`0x00222008` is a 16-byte request: pid (4), length (4), address (8).

## Build

Requires Visual Studio 2022 and the Windows Driver Kit.

```
pwsh -File build.ps1
```

Output: `build/RltRead.sys`.

## Install

```
sc create RltTele type= kernel start= demand binPath= \SystemRoot\System32\drivers\RltTele.sys
sc start RltTele
```

## Notes

* Read-only: no writes, no hooks, no callbacks.
* Works on processes whose handles are protected by an anti-cheat.
* Signed releases are produced from this source; see the releases page.
