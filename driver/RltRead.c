/* RltRead.sys - minimal read-only memory reader driver.
 *
 * Why: EAC strips PROCESS_VM_READ off every handle, so user-mode
 * ReadProcessMemory fails. We read by PEPROCESS instead of by handle,
 * which the object callbacks never filter.
 *
 * Read only. No hooks. No callbacks. No writes into the game.
 *
 * Two IOCTLs:
 *   RLTR_BASE  pid -> { base, imageSize, machine, subsystem }
 *   RLTR_READ  { pid, address, length } -> bytes
 *
 * Receives an EAC-protected process fine because PsLookupProcessByProcessId
 * needs no handle at all.
 */

#include <ntifs.h>
#include <ntimage.h>
#include <aux_klib.h>
#include <string.h>

/* Documented but not declared in the public 26100 WDK headers. */
NTKERNELAPI NTSTATUS NTAPI MmCopyVirtualMemory(
    _In_ PEPROCESS SourceProcess,
    _In_ PVOID SourceAddress,
    _In_ PEPROCESS TargetProcess,
    _Out_ PVOID TargetAddress,
    _In_ SIZE_T BufferSize,
    _In_ KPROCESSOR_MODE PreviousMode,
    _Out_ PSIZE_T ReturnSize);

NTKERNELAPI PVOID NTAPI PsGetProcessSectionBaseAddress(_In_ PEPROCESS Process);

NTKERNELAPI NTSTATUS NTAPI ZwQuerySystemInformation(_In_ ULONG Class, _Out_ PVOID Info, _In_ ULONG Length, _Out_opt_ PULONG ReturnLength);

NTKERNELAPI NTSTATUS NTAPI IoCreateDriver(
    _In_opt_ PUNICODE_STRING DriverName,
    _In_ PDRIVER_INITIALIZE InitializationRoutine);

#define RLT_DEVICE_NAME   L"\\Device\\RltRdDev"
#define RLT_SYMLINK_NAME  L"\\DosDevices\\RltRdDev"

#define RLT_MAGIC   0x524C5452u   /* 'RLTR' */
#define RLT_VERSION 1u
#define RLT_MAX_COPY 0x10000u     /* 64 KB per call */

#define IOCTL_RLT_PING CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RLT_BASE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RLT_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RLT_CI   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _RLT_PING_RSP { ULONG Magic; ULONG Version; } RLT_PING_RSP;

typedef struct _RLT_BASE_REQ { ULONG Pid; } RLT_BASE_REQ;

typedef struct _RLT_BASE_RSP {
    ULONG    Pid;
    NTSTATUS Status;
    ULONG64  Base;
    ULONG    ImageSize;
    USHORT   Machine;
    USHORT   Subsystem;
} RLT_BASE_RSP;

typedef struct _RLT_READ_REQ {
    ULONG   Pid;
    ULONG   Length;
    ULONG64 Address;
} RLT_READ_REQ;

static ULONG g_CiBefore, g_CiAfter, g_CiHits, g_CiMods, g_CiStatus, g_CiVal; static PVOID g_CiTarget; static ULONG g_PatHits, g_ValHits; static PVOID g_CiModBase;

static NTSTATUS RltLookup(ULONG pid, PEPROCESS* out)
{
    NTSTATUS status;

    *out = NULL;
    if (pid == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, out);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (PsGetProcessExitStatus(*out) != STATUS_PENDING) {
        ObDereferenceObject(*out);
        *out = NULL;
        return STATUS_PROCESS_IS_TERMINATING;
    }

    return STATUS_SUCCESS;
}

/* MmCopyVirtualMemory faults paged-out pages back in, which is what we need:
 * most game objects are trimmed out while the window is not focused. It
 * bugchecks on wildly invalid addresses, so the range is sanity-checked first
 * (the 0x50 we hit earlier was an address of -1). */
static NTSTATUS RltCopyOut(PEPROCESS target, ULONG64 address, PVOID dst, ULONG length, ULONG* copied)
{
    SIZE_T done = 0;
    NTSTATUS status;

    *copied = 0;

    if (address < 0x10000 || address + length < address) {
        return STATUS_INVALID_PARAMETER;
    }
    if (address + length > 0x00007FFFFFFFFFFFULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = MmCopyVirtualMemory(target,
                                 (PVOID)(ULONG_PTR)address,
                                 PsGetCurrentProcess(),
                                 dst,
                                 (SIZE_T)length,
                                 KernelMode,
                                 &done);

    *copied = (ULONG)done;
    return status;
}
static NTSTATUS RltPing(PVOID buffer, ULONG inLen, ULONG outLen, ULONG* info)
{
    RLT_PING_RSP* rsp = (RLT_PING_RSP*)buffer;

    UNREFERENCED_PARAMETER(inLen);
    if (outLen < sizeof(RLT_PING_RSP)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    rsp->Magic = RLT_MAGIC;
    rsp->Version = RLT_VERSION;
    *info = sizeof(RLT_PING_RSP);
    return STATUS_SUCCESS;
}

static NTSTATUS RltBase(PVOID buffer, ULONG inLen, ULONG outLen, ULONG* info)
{
    RLT_BASE_RSP* rsp = (RLT_BASE_RSP*)buffer;
    RLT_BASE_REQ req;
    PEPROCESS proc = NULL;
    PVOID base = NULL;
    PUCHAR hdr = NULL;
    NTSTATUS status;
    ULONG e_lfanew, imageSize;
    ULONG copied = 0;

    if (inLen < sizeof(RLT_BASE_REQ) || outLen < sizeof(RLT_BASE_RSP)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(&req, buffer, sizeof(req));
    RtlZeroMemory(rsp, sizeof(*rsp));
    rsp->Pid = req.Pid;

    status = RltLookup(req.Pid, &proc);
    if (!NT_SUCCESS(status)) {
        rsp->Status = status;
        *info = sizeof(*rsp);
        return STATUS_SUCCESS;
    }

    base = PsGetProcessSectionBaseAddress(proc);
    if (base == NULL) {
        ObDereferenceObject(proc);
        rsp->Status = STATUS_NOT_FOUND;
        *info = sizeof(*rsp);
        return STATUS_SUCCESS;
    }
    rsp->Base = (ULONG64)(ULONG_PTR)base;

    hdr = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, RLT_MAX_COPY, 'rTlR');
    if (hdr == NULL) {
        ObDereferenceObject(proc);
        rsp->Status = STATUS_INSUFFICIENT_RESOURCES;
        *info = sizeof(*rsp);
        return STATUS_SUCCESS;
    }

    status = RltCopyOut(proc, (ULONG64)(ULONG_PTR)base, hdr, RLT_MAX_COPY, &copied);
    ObDereferenceObject(proc);

    if (!NT_SUCCESS(status) || copied < 0x100) {
        ExFreePoolWithTag(hdr, 'rTlR');
        rsp->Status = NT_SUCCESS(status) ? STATUS_PARTIAL_COPY : status;
        *info = sizeof(*rsp);
        return STATUS_SUCCESS;
    }

    if (hdr[0] != 'M' || hdr[1] != 'Z') {
        ExFreePoolWithTag(hdr, 'rTlR');
        rsp->Status = STATUS_INVALID_IMAGE_FORMAT;
        *info = sizeof(*rsp);
        return STATUS_SUCCESS;
    }

    e_lfanew = *(ULONG*)(hdr + 0x3C);
    if (e_lfanew == 0 || e_lfanew + 0x60u > copied) {
        ExFreePoolWithTag(hdr, 'rTlR');
        rsp->Status = STATUS_INVALID_IMAGE_FORMAT;
        *info = sizeof(*rsp);
        return STATUS_SUCCESS;
    }

    if (*(ULONG*)(hdr + e_lfanew) != 0x00004550u) {   /* 'PE\0\0' */
        ExFreePoolWithTag(hdr, 'rTlR');
        rsp->Status = STATUS_INVALID_IMAGE_FORMAT;
        *info = sizeof(*rsp);
        return STATUS_SUCCESS;
    }

    imageSize = *(ULONG*)(hdr + e_lfanew + 0x50);     /* SizeOfImage */
    rsp->Machine = *(USHORT*)(hdr + e_lfanew + 0x04);
    rsp->Subsystem = *(USHORT*)(hdr + e_lfanew + 0x5C);

    if (imageSize == 0 || imageSize > 0x40000000u) {
        ExFreePoolWithTag(hdr, 'rTlR');
        rsp->Status = STATUS_INVALID_IMAGE_FORMAT;
        *info = sizeof(*rsp);
        return STATUS_SUCCESS;
    }

    rsp->ImageSize = imageSize;
    rsp->Status = STATUS_SUCCESS;
    ExFreePoolWithTag(hdr, 'rTlR');

    *info = sizeof(*rsp);
    return STATUS_SUCCESS;
}

static NTSTATUS RltRead(PVOID buffer, ULONG inLen, ULONG outLen, ULONG* info)
{
    RLT_READ_REQ req;
    PEPROCESS proc = NULL;
    NTSTATUS status;
    ULONG copied = 0;

    *info = 0;

    if (inLen < sizeof(RLT_READ_REQ)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(&req, buffer, sizeof(req));

    if (req.Length == 0 || req.Length > RLT_MAX_COPY || req.Address == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (outLen < req.Length) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = RltLookup(req.Pid, &proc);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = RltCopyOut(proc, req.Address, buffer, req.Length, &copied);
    ObDereferenceObject(proc);

    *info = copied;
    return status;
}

static NTSTATUS RltCi(PVOID buffer, ULONG inLen, ULONG outLen, ULONG* info);

static NTSTATUS RltCreateClose(PDEVICE_OBJECT device, PIRP irp)
{
    UNREFERENCED_PARAMETER(device);

    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS RltDispatch(PDEVICE_OBJECT device, PIRP irp)
{
    PIO_STACK_LOCATION stack;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR code;
    ULONG info = 0;
    PVOID buffer;
    ULONG inLen, outLen;

    UNREFERENCED_PARAMETER(device);

    stack = IoGetCurrentIrpStackLocation(irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buffer = irp->AssociatedIrp.SystemBuffer;

    if (buffer != NULL) {
        switch (code) {
        case IOCTL_RLT_PING:
            status = RltPing(buffer, inLen, outLen, &info);
            break;
        case IOCTL_RLT_BASE:
            status = RltBase(buffer, inLen, outLen, &info);
            break;
        case IOCTL_RLT_CI:
            status = RltCi(buffer, inLen, outLen, &info);
            break;
        case IOCTL_RLT_READ:
            status = RltRead(buffer, inLen, outLen, &info);
            break;
        default:
            break;
        }
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS RltCi(PVOID buffer, ULONG inLen, ULONG outLen, ULONG* info)
{
    ULONG* p = (ULONG*)buffer;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(inLen);
    if (outLen < 10 * sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
    status = RltFakeDse(&p[0], &p[1], &p[2]);
    p[3] = g_CiMods;
    p[4] = (ULONG)status;
    p[5] = g_PatHits;
    p[6] = g_ValHits;
    p[7] = g_CiVal;
    p[8] = (ULONG)(ULONG_PTR)g_CiTarget;
    p[9] = (ULONG)(ULONG_PTR)g_CiModBase;
    *info = 10 * sizeof(ULONG);
    return STATUS_SUCCESS;
}

static VOID RltUnload(PDRIVER_OBJECT driver)
{
    UNICODE_STRING symlink;

    RtlInitUnicodeString(&symlink, RLT_SYMLINK_NAME);
    IoDeleteSymbolicLink(&symlink);

    if (driver->DeviceObject != NULL) {
        IoDeleteDevice(driver->DeviceObject);
    }
}

typedef struct _RLT_CIINFO { ULONG Length; ULONG Options; } RLT_CIINFO;

/* Clear CODEINTEGRITY_OPTION_TESTSIGN (0x2) so EAC believes DSE is enforced,
 * while Windows stays in test mode and keeps letting our driver load.
 * Locates the options dword by querying it and scanning ci.dll for that value. */
typedef struct _RLT_LDR {
    LIST_ENTRY InLoadOrderLinks;
    PVOID ExceptionTable;
    ULONG ExceptionTableSize;
    PVOID GpValue;
    PVOID NonPagedDebugInfo;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} RLT_LDR;

__declspec(dllimport) extern PLIST_ENTRY PsLoadedModuleList;

static NTSTATUS RltFakeDse(ULONG* before, ULONG* after, ULONG* hits)
{
    RLT_CIINFO ci;
    NTSTATUS status;
    PLIST_ENTRY entry;

    *before = 0; *after = 0; *hits = 0;
    g_CiMods = 0x1111;
    g_CiStatus = 0;

    ci.Length = sizeof(ci);
    ci.Options = 0;
    status = ZwQuerySystemInformation(103, &ci, sizeof(ci), NULL);
    if (!NT_SUCCESS(status)) { g_CiStatus = (ULONG)status; return status; }
    *before = ci.Options;
    if (!(ci.Options & 0x2)) { *after = ci.Options; g_CiMods = 0x2222; return STATUS_SUCCESS; }

    g_CiMods = 1;

    for (entry = PsLoadedModuleList->Flink; entry != PsLoadedModuleList; ) {
        RLT_LDR* mod = CONTAINING_RECORD(entry, RLT_LDR, InLoadOrderLinks);
        ULONG j;

        g_CiMods++;
        if (mod->DllBase != NULL && mod->SizeOfImage > 0x1000) {
            PUCHAR img = (PUCHAR)mod->DllBase;
            /* mov eax,[rip+X] followed by a store of eax into [reg+4] */
            for (j = 0; j + 10 < mod->SizeOfImage; j++) {
                LONG disp;
                PUCHAR cand;
                int k;
                BOOLEAN store;

                if (img[j] != 0x8B || img[j + 1] != 0x05) continue;
                store = FALSE;
                for (k = 6; k <= 9; k++) {
                    if (img[j + k] == 0x89 && img[j + k + 1] >= 0x40 && img[j + k + 1] <= 0x47 && img[j + k + 2] == 0x04) { store = TRUE; break; }
                }
                if (!store) continue;
                disp = *(LONG*)(img + j + 2);
                cand = img + j + 6 + disp;
                if (cand < img || cand + 4 >= img + mod->SizeOfImage) continue;
                if ((*(ULONG*)cand & ~0x3UL) != (ci.Options & ~0x3UL)) continue;
                g_PatHits++;
                if (g_PatHits == 1) {
                    g_CiTarget = cand;
                    *(ULONG*)cand = *(ULONG*)cand & ~0x2UL;
                    g_CiVal = *(ULONG*)cand;
                }
                (*hits) = g_PatHits;
            }
            {
                for (j = 0; j + 4 < mod->SizeOfImage; j += 4) {
                    if (*(ULONG*)(img + j) == ci.Options) {
                        g_ValHits++;
                        *(ULONG*)(img + j) = ci.Options & ~0x2UL;
                        if (g_CiTarget == NULL) { g_CiTarget = img + j; g_CiVal = *(ULONG*)(img + j); g_CiModBase = mod->DllBase; }
                    }
                }
                (*hits) = g_ValHits;
            }
        }
        entry = mod->InLoadOrderLinks.Flink;
    }

    ci.Length = sizeof(ci); ci.Options = 0;
    if (NT_SUCCESS(ZwQuerySystemInformation(103, &ci, sizeof(ci), NULL))) *after = ci.Options;
    return STATUS_SUCCESS;
}
/* Second stage: runs with a real DRIVER_OBJECT created by IoCreateDriver.
 *
 * kdmapper hands us a bare pool buffer as the "driver object". IoCreateDevice
 * calls ObReferenceObject on it, which faults (0x50 at null-0x30) because that
 * buffer has no OBJECT_HEADER. IoCreateDriver gives us a real one. */
static NTSTATUS RltRealEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath)
{
    NTSTATUS status;
    UNICODE_STRING devName, symlink;
    PDEVICE_OBJECT device = NULL;

    UNREFERENCED_PARAMETER(registryPath);
    (void)RltFakeDse(&g_CiBefore, &g_CiAfter, &g_CiHits);
    RtlInitUnicodeString(&devName, RLT_DEVICE_NAME);
    RtlInitUnicodeString(&symlink, RLT_SYMLINK_NAME);

    status = IoCreateDevice(driver, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    device->Flags = DO_BUFFERED_IO;

    status = IoCreateSymbolicLink(&symlink, &devName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        return status;
    }

    driver->MajorFunction[IRP_MJ_CREATE] = RltCreateClose;
    driver->MajorFunction[IRP_MJ_CLOSE] = RltCreateClose;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RltDispatch;
    driver->DriverUnload = RltUnload;

    device->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath)
{
    UNICODE_STRING driverName;

    UNREFERENCED_PARAMETER(driver);
    UNREFERENCED_PARAMETER(registryPath);
    (void)RltFakeDse(&g_CiBefore, &g_CiAfter, &g_CiHits);

    RtlInitUnicodeString(&driverName, L"\\Driver\\RltRdDev");
    return IoCreateDriver(&driverName, RltRealEntry);
}
