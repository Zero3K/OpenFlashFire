/*
    OpenFlashFire: Buffer Incorporated Write Reordering Layer
    Copyright (C) 2009-2010, Hyojun Kim

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __FFIRE_H__
#define __FFIRE_H__

#define FFIRE_VERSION               (0xf0000)
#define FFIRE_IOCTL_INFO            (0x7dc00)
#define FFIRE_IOCTL_ENABLE          (0x7dc01)
#define FFIRE_IOCTL_DISABLE         (0x7dc02)
#define FFIRE_IOCTL_FLUSH           (0x7dc03)
#define FFIRE_IOCTL_DEADLINE        (0x7dc04)


#define FFIRE_1MEGA                 (1024 * 1024)
#define FFIRE_DEFAULT_DISKNUM       (0)
#define FFIRE_DEFAULT_BUFFERSIZE    (32)
#define FFIRE_DEFAULT_CLUSTERSIZE   (4092)
#define FFIRE_DEFAULT_OPTION        (0)


#define FFIRE_TAG                   'fire'
#define FFIRE_REGPATH               \
 L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\offire\\Parameters"
#define FFIRE_KEY_DISKNUM           L"DiskNum"
#define FFIRE_KEY_BUFFERSIZE        L"BufferSize"
#define FFIRE_KEY_CLUSTERSIZE       L"ClusterSize"
#define FFIRE_KEY_OPTION            L"Option"

#define FFIRE_MIN_BUFFERSIZE        (8)

#define FFIRE_OPTION_DISABLE        (1 << 0)
#define FFIRE_OPTION_NOFLUSH        (1 << 1)
#define FFIRE_OPTION_NODEADLINE     (1 << 2)

#define FFIRE_TIMER_TICK            (200)
#define FFIRE_TIMER_TICKCNT         (3)
#define FFIRE_TIMER_TICKPERSECOND   (1000)
#define FFIRE_DEADLINE              (5 * FFIRE_TIMER_TICKPERSECOND)

#define FFIRE_DISABLE_USER          (1 << 0)
#define FFIRE_DISABLE_POWER         (1 << 1)
#define FFIRE_DISABLE_REMOVE        (1 << 2)


#ifndef __FFIRE_MONITOR__

typedef struct _DEVICE_EXTENSION
{
    PDEVICE_OBJECT      pDevObj;
    PDEVICE_OBJECT      pTgtDevObj;
    PDEVICE_OBJECT      pPhyDevObj;

    ULONG               nDiskNum;
    ULONG               nBufferSize;
    ULONG               nClusterSize;
    ULONG               nOption;

    KEVENT              evPagingPathCount;
    ULONG               nPagingPathCount;

    KTIMER              tiTickTimer;

    LIST_ENTRY          listIrpHead;
    KSPIN_LOCK          lockIrpList;
    KEVENT              evIrpReq;
    KEVENT              evFlushed;

    PVOID               fpIrpThread;
    BOOLEAN             bTerminateThread;
    BOOLEAN             bTriggerFlush;

    ULONG               nNumOfScts;
    ULONG               nSctSize;
    PCHAR               pWBMem;
    PVOID               pCCtx;

    BOOLEAN             bBypassOn;

    ULONG               nDisabled;
    WJOB                stJob;

    ULONG               nCurTime;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

#define DEVICE_EXTENSION_SIZE       sizeof(DEVICE_EXTENSION)

#define FILTER_DEVICE_PROPOGATE_FLAGS            0
#define FILTER_DEVICE_PROPOGATE_CHARACTERISTICS (FILE_REMOVABLE_MEDIA  | \
                                                 FILE_READ_ONLY_DEVICE | \
                                                 FILE_FLOPPY_DISKETTE)

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);

NTSTATUS FFIREAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT pPhyDevObj);
NTSTATUS FFIREDispatchPnp(PDEVICE_OBJECT pDevObj, PIRP Irp);
NTSTATUS FFIREDispatchPower(PDEVICE_OBJECT pDevObj, PIRP Irp);
NTSTATUS FFIREBypassIrp(PDEVICE_OBJECT pDevObj, PIRP Irp);
NTSTATUS FFIRETriggerThread(PDEVICE_OBJECT pDevObj, PIRP Irp);
VOID     FFIREUnload(PDRIVER_OBJECT DriverObject);
NTSTATUS FFIREDeviceControl(PDEVICE_OBJECT pDevObj, PIRP Irp);

static NTSTATUS _InitWriteBufer(PDEVICE_EXTENSION pDevExt);
static VOID     _SyncFilterWithTarget(PDEVICE_OBJECT FilterDevice, PDEVICE_OBJECT TargetDevice);
static NTSTATUS _StartDevice(PDEVICE_OBJECT pDevObj, PIRP Irp);
static NTSTATUS _RemoveDevice(PDEVICE_OBJECT pDevObj, PIRP Irp);
static NTSTATUS _DeviceUsageNotification(PDEVICE_OBJECT pDevObj, PIRP Irp);
static NTSTATUS _IrpCompletion(PDEVICE_OBJECT pDevObj, PIRP Irp, PVOID Context);
static NTSTATUS _ForwardIrpSynchronous(PDEVICE_EXTENSION pDevExt, PIRP Irp);
static void     _DoFlush(PDEVICE_EXTENSION pDevExt, U32 nCnt);
static BOOLEAN  _DoFlushDeadlined(PDEVICE_EXTENSION pDevExt, U32 nDeadline);
static NTSTATUS _IOCtlBlk(PDEVICE_EXTENSION pDevExt, ULONG nCode, PVOID pIBuf, ULONG nIBufSize, PVOID pOBuf, PULONG pOBufSize);
static NTSTATUS _ReadBlk(PDEVICE_EXTENSION pDevExt, ULONG nLsn, ULONG nNumOfSct, PVOID pBuffer);
static NTSTATUS _WriteBlk(PDEVICE_EXTENSION pDevExt, ULONG nLsn, ULONG nNumOfSct, PVOID pBuffer);
static U32      _GetTick(void);
static NTSTATUS _WriteRegParam(PCWSTR pKey, ULONG nValue);
static NTSTATUS _ReadRegParam(PWSTR pKey, PULONG pValue);
static BOOLEAN  _HandleReadIrp(PDEVICE_EXTENSION pDevExt, ULONG nLsn, ULONG nNumOfScts, PCHAR pSysBuf);
static BOOLEAN  _HandleWriteIrp(PDEVICE_EXTENSION pDevExt, ULONG nLsn, ULONG nNumOfScts, PCHAR pSysBuf);
static BOOLEAN  _HandleRWIrp(PIRP pIrp, PIO_STACK_LOCATION pIOStk, PDEVICE_EXTENSION pDevExt);
static BOOLEAN  _HandleDeadLine(PDEVICE_EXTENSION pDevExt);
static VOID     _IrpHandlingThread(PVOID Context);

#if DBG
#define DebugPrint(x)   KdPrint(x)
#else
#define DebugPrint(x)
#endif /* DBG */

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)

#pragma alloc_text (PAGE, FFIREAddDevice)
#pragma alloc_text (PAGE, FFIREDispatchPnp)
#pragma alloc_text (PAGE, FFIREUnload)
#pragma alloc_text (PAGE, _InitWriteBufer)
#pragma alloc_text (PAGE, _StartDevice)
#pragma alloc_text (PAGE, _RemoveDevice)
#endif

#endif /* __FFIRE_MONITOR__ */

#endif /* __FFIRE_H__ */