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


#include "ntddk.h"
#include "ntdddisk.h"
#include "stdarg.h"
#include "stdio.h"

#include <ntddvol.h>
#include <mountdev.h>

#include "ffbuf.h"
#include "offire.h"


/*****************************************************************************
 *                           Interface Functions
 *****************************************************************************/

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ULONG nIdx;

    DebugPrint(("#### FlashFire driverhas been loaded #### \r\n"));

    /* Set default handlers */
    for (nIdx = 0; nIdx <= IRP_MJ_MAXIMUM_FUNCTION; nIdx++)
    {
        DriverObject->MajorFunction[nIdx] = FFIREBypassIrp;
    }

    DriverObject->MajorFunction[IRP_MJ_READ          ] = FFIRETriggerThread;
    DriverObject->MajorFunction[IRP_MJ_WRITE         ] = FFIRETriggerThread;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN      ] = FFIRETriggerThread;
    DriverObject->MajorFunction[IRP_MJ_PNP           ] = FFIREDispatchPnp  ;
    DriverObject->MajorFunction[IRP_MJ_POWER         ] = FFIREDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS ] = FFIRETriggerThread;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = FFIREDeviceControl;

    DriverObject->DriverExtension->AddDevice           = FFIREAddDevice;
    DriverObject->DriverUnload                         = FFIREUnload;

    return STATUS_SUCCESS;
}

NTSTATUS
FFIREAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT pPhyDevObj)
{
    static ULONG        nAddedCnt;
    PDEVICE_OBJECT      pFilterDevObj;
    PDEVICE_EXTENSION   pDevExt;
    HANDLE              hThread;
    NTSTATUS            nRet;

    ULONG               nDiskNum;
    ULONG               nBufferSize;
    ULONG               nClusterSize;
    ULONG               nOption;
    PCHAR               pBuffer;

    PAGED_CODE();

    DebugPrint(("+++ FFIREAddDevice +++ \r\n"));

    nAddedCnt++;
    do
    {
        nRet = _ReadRegParam(FFIRE_KEY_DISKNUM, &nDiskNum);
        if (!NT_SUCCESS(nRet)) nDiskNum = FFIRE_DEFAULT_DISKNUM;
        if ((nDiskNum + 1) != nAddedCnt) break;

        nRet = _ReadRegParam(FFIRE_KEY_BUFFERSIZE, &nBufferSize);
        if (!NT_SUCCESS(nRet)) nBufferSize = FFIRE_DEFAULT_BUFFERSIZE;

        pBuffer = NULL;
        while (nBufferSize >= FFIRE_MIN_BUFFERSIZE)
        {
            pBuffer = (PCHAR)ExAllocatePoolWithTag(NonPagedPool,
                             nBufferSize * FFIRE_1MEGA, FFIRE_TAG);
            if (pBuffer) break;
            nBufferSize--;
        }
        if (pBuffer == NULL) break;
        DebugPrint(("- Allcated Buffer Size = %d Mbytes\r\n", nBufferSize));

        nRet = _ReadRegParam(FFIRE_KEY_CLUSTERSIZE, &nClusterSize);
        if (!NT_SUCCESS(nRet)) nClusterSize = FFIRE_DEFAULT_CLUSTERSIZE;

        nRet = _ReadRegParam(FFIRE_KEY_OPTION, &nOption);
        if (!NT_SUCCESS(nRet)) nOption = FFIRE_DEFAULT_OPTION;


        nRet = IoCreateDevice(DriverObject,
                              DEVICE_EXTENSION_SIZE,
                              NULL,
                              FILE_DEVICE_DISK,
                              FILE_DEVICE_SECURE_OPEN,
                              FALSE,
                              &pFilterDevObj);
        if (!NT_SUCCESS(nRet))
        {
            DebugPrint(("FFIREAddDevice: Cannot create pFilterDevObj\r\n"));
            break;
        }
        pFilterDevObj->Flags |= DO_DIRECT_IO;

        pDevExt = (PDEVICE_EXTENSION)pFilterDevObj->DeviceExtension;
        RtlZeroMemory(pDevExt, DEVICE_EXTENSION_SIZE);

        pDevExt->pPhyDevObj   = pPhyDevObj;
        pDevExt->nDiskNum     = nDiskNum;
        pDevExt->nBufferSize  = nBufferSize;
        pDevExt->nClusterSize = nClusterSize;
        pDevExt->nOption      = nOption;
        pDevExt->pWBMem       = pBuffer;

        pDevExt->pTgtDevObj =
            IoAttachDeviceToDeviceStack(pFilterDevObj, pPhyDevObj);
        if (pDevExt->pTgtDevObj == NULL)
        {
            ExFreePool(pBuffer);
            IoDeleteDevice(pFilterDevObj);
            nRet = STATUS_NO_SUCH_DEVICE;
            break;
        }

        pDevExt->pDevObj = pFilterDevObj;
        KeInitializeEvent(&pDevExt->evPagingPathCount,
                          NotificationEvent, TRUE);

        pFilterDevObj->Flags |=  DO_POWER_PAGABLE;
        pFilterDevObj->Flags &= ~DO_DEVICE_INITIALIZING;

        InitializeListHead(&pDevExt->listIrpHead);
        KeInitializeSpinLock(&pDevExt->lockIrpList);
        KeInitializeEvent(&pDevExt->evIrpReq, SynchronizationEvent, FALSE);
        KeInitializeEvent(&pDevExt->evFlushed, SynchronizationEvent, FALSE);
        pDevExt->bTerminateThread = FALSE;

        PsCreateSystemThread(&hThread, (ACCESS_MASK) 0L,
                     NULL, NULL, NULL, _IrpHandlingThread, pDevExt->pDevObj);

        ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, NULL,
                              KernelMode, &pDevExt->fpIrpThread, NULL);
        KeInitializeTimerEx(&pDevExt->tiTickTimer, SynchronizationTimer);

    } while (0);

    DebugPrint(("--- FFIREAddDevice --- \r\n"));

    return STATUS_SUCCESS;
}

NTSTATUS
FFIREDispatchPnp(PDEVICE_OBJECT pDevObj, PIRP Irp)
{
    PIO_STACK_LOCATION pIOStk;
    PDEVICE_EXTENSION  pDevExt;
    NTSTATUS           nRet;

    PAGED_CODE();

    pIOStk = IoGetCurrentIrpStackLocation(Irp);

    switch (pIOStk->MinorFunction)
    {
    case IRP_MN_START_DEVICE:
        nRet = _StartDevice(pDevObj, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        nRet = _RemoveDevice(pDevObj, Irp);
        break;

    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        nRet = _DeviceUsageNotification(pDevObj, Irp);
        break;

    default:
        nRet = FFIREBypassIrp(pDevObj, Irp);
        break;
    }
    return nRet;
}

NTSTATUS
FFIREBypassIrp(PDEVICE_OBJECT pDevObj, PIRP Irp)
{
    PDEVICE_EXTENSION   pDevExt;

    IoSkipCurrentIrpStackLocation(Irp);
    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    return IoCallDriver(pDevExt->pTgtDevObj, Irp);
}

NTSTATUS
FFIREDispatchPower(PDEVICE_OBJECT pDevObj, PIRP Irp)
{
    PDEVICE_EXTENSION  pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;
    PIO_STACK_LOCATION pIOStk;

    pIOStk = IoGetCurrentIrpStackLocation(Irp);

    if (pIOStk->MinorFunction == IRP_MN_SET_POWER)
    {
        if (pIOStk->Parameters.Power.Type == SystemPowerState)
        {
            if (pIOStk->Parameters.Power.State.SystemState >=
                                            PowerSystemHibernate)
            {
                if (pDevExt->nDisabled == 0)
                {
                    pDevExt->bTriggerFlush = TRUE;
                    KeSetEvent(&pDevExt->evIrpReq, IO_DISK_INCREMENT, TRUE);
                    KeWaitForSingleObject(&pDevExt->evFlushed, Executive,
                                          KernelMode, FALSE, NULL);
                }
                pDevExt->nDisabled |= FFIRE_DISABLE_POWER;
                DebugPrint((":: FlashFire is disabled !!! ::\n"));
            }
            else
            {
                pDevExt->nDisabled &= ~FFIRE_DISABLE_POWER;
                DebugPrint((":: FlashFire is enabled !!! ::\n"));
            }
        }
        else if (pIOStk->Parameters.Power.Type == DevicePowerState)
        {
            if ((pIOStk->Parameters.Power.State.DeviceState == PowerDeviceD0)
                && (pDevExt->nDisabled & FFIRE_DISABLE_POWER))
            {
                pDevExt->nDisabled &= ~FFIRE_DISABLE_POWER;
                DebugPrint((":: FlashFire is enabled !!! ::\n"));
            }
        }
    }

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);

    return PoCallDriver(pDevExt->pTgtDevObj, Irp);
}


NTSTATUS
FFIRETriggerThread(PDEVICE_OBJECT pDevObj, PIRP Irp)
{
    PDEVICE_EXTENSION   pDevExt;
    PIO_STACK_LOCATION  pIOStk;

    pDevExt = (PDEVICE_EXTENSION)pDevObj->DeviceExtension;
    if ((pDevExt->bBypassOn) && (pDevExt->nDisabled))
    {
        return FFIREBypassIrp(pDevObj, Irp);
    }
    if (pDevExt->nDisabled == 0) pDevExt->bBypassOn = FALSE;

    pIOStk  = IoGetCurrentIrpStackLocation(Irp);
    IoMarkIrpPending(Irp);
    ExInterlockedInsertTailList(&pDevExt->listIrpHead,
                                &Irp->Tail.Overlay.ListEntry,
                                &pDevExt->lockIrpList);
    KeSetEvent(&pDevExt->evIrpReq, (KPRIORITY)0, FALSE);

    return STATUS_PENDING;
}

VOID
FFIREUnload(PDRIVER_OBJECT DriverObject)
{
    PDEVICE_OBJECT    pDevObj;
    PDEVICE_EXTENSION pDevExt;

    PAGED_CODE();

    pDevObj = DriverObject->DeviceObject;
    pDevExt = (PDEVICE_EXTENSION)pDevObj->DeviceExtension;

    pDevExt->bTerminateThread = TRUE;

    KeSetEvent(&pDevExt->evIrpReq, (KPRIORITY) 0, FALSE);

    KeWaitForSingleObject(pDevExt->fpIrpThread, Executive,
                          KernelMode, FALSE, NULL);
    ObDereferenceObject(pDevExt->fpIrpThread);

}

NTSTATUS
FFIREDeviceControl(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PIO_STACK_LOCATION  pIOStk;
    PDEVICE_EXTENSION   pDevExt;
    FFIREINFO*          pInfo;
    NTSTATUS            nRet;
    BOOLEAN             bHandled;

    pDevExt  = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;
    pIOStk   = IoGetCurrentIrpStackLocation(pIrp);
    bHandled = TRUE;

    switch (pIOStk->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_DISK_SET_PARTITION_INFO:
    case IOCTL_DISK_SET_PARTITION_INFO_EX:
        break;

    case FFIRE_IOCTL_INFO:
        if (pIOStk->Parameters.DeviceIoControl.OutputBufferLength <
                                                    sizeof(FFIREINFO))
        {
            pIrp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
            pIrp->IoStatus.Information = 0;
        }
        else
        {
            pInfo = (FFIREINFO*)pIrp->AssociatedIrp.SystemBuffer;
            RtlZeroMemory(pInfo, sizeof(FFIREINFO));

            pInfo->nVersion    = FFIRE_VERSION;
            pInfo->nDiskNum    = pDevExt->nDiskNum;
            pInfo->nOption     = pDevExt->nOption;
            pInfo->nDisabled   = pDevExt->nDisabled;
            pInfo->nDeadline   = FFIRE_DEADLINE / FFIRE_TIMER_TICKPERSECOND;

            RtlCopyMemory(&pInfo->stCtx, pDevExt->pCCtx, sizeof(FFBCTX));
            pIrp->IoStatus.Information = sizeof(FFIREINFO);
        }
        break;

    case FFIRE_IOCTL_DISABLE:
        if (pDevExt->nDisabled == 0)
        {
            pDevExt->bTriggerFlush = TRUE;
            KeSetEvent(&pDevExt->evIrpReq, IO_DISK_INCREMENT, TRUE);
            KeWaitForSingleObject(&pDevExt->evFlushed, Executive,
                                  KernelMode, FALSE, NULL);
        }
        pDevExt->nDisabled |= FFIRE_DISABLE_USER;

        pDevExt->nOption |= FFIRE_OPTION_DISABLE;
        _WriteRegParam(FFIRE_KEY_OPTION, pDevExt->nOption);
        break;

    case FFIRE_IOCTL_ENABLE:
        if ((pDevExt->nDisabled & FFIRE_DISABLE_USER))
        {
            pDevExt->nDisabled &= ~FFIRE_DISABLE_USER;

            pDevExt->nOption &= ~FFIRE_OPTION_DISABLE;
            _WriteRegParam(FFIRE_KEY_OPTION, pDevExt->nOption);
        }
        break;

    case FFIRE_IOCTL_FLUSH:
        pDevExt->nOption ^= FFIRE_OPTION_NOFLUSH;
        _WriteRegParam(FFIRE_KEY_OPTION, pDevExt->nOption);
        break;

    case FFIRE_IOCTL_DEADLINE:
        pDevExt->nOption ^= FFIRE_OPTION_NODEADLINE;
        _WriteRegParam(FFIRE_KEY_OPTION, pDevExt->nOption);
        break;

    default:
        bHandled = FALSE;
    }

    if (bHandled)
    {
        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
        pIrp->IoStatus.Status      = STATUS_SUCCESS;
        pIrp->IoStatus.Information = 0;
        nRet                       = STATUS_SUCCESS;
    }
    else
    {
        nRet = FFIREBypassIrp(pDevObj, pIrp);
    }

    return nRet;
}


/*****************************************************************************
 *                           Internal Functions
 *****************************************************************************/

static NTSTATUS
_InitWriteBufer(PDEVICE_EXTENSION pDevExt)
{
    DISK_GEOMETRY stDiskGeometry;
    ULONG         nOutSize;
    NTSTATUS      nRet;
    U32           nCltSize;

    PAGED_CODE();
    do
    {
        nOutSize = sizeof(stDiskGeometry);
        nRet = _IOCtlBlk(pDevExt, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                         NULL, 0, &stDiskGeometry, &nOutSize);
        if (!NT_SUCCESS(nRet)) break;

        pDevExt->nSctSize   = (U32) stDiskGeometry.BytesPerSector;
        pDevExt->nNumOfScts = (U32)(stDiskGeometry.TracksPerCylinder *
                                    stDiskGeometry.SectorsPerTrack *
                                    stDiskGeometry.Cylinders.QuadPart);

        if (pDevExt->nOption & FFIRE_OPTION_DISABLE)
        {
            pDevExt->nDisabled |= FFIRE_DISABLE_USER;
        }

        pDevExt->stJob.nBufSize = pDevExt->nClusterSize * pDevExt->nSctSize;
        pDevExt->stJob.pBuf = pDevExt->pWBMem +
            (pDevExt->nBufferSize * FFIRE_1MEGA) - pDevExt->stJob.nBufSize;

        pDevExt->pCCtx =
            FFBUF_Init(pDevExt->pWBMem,
                 pDevExt->nBufferSize * FFIRE_1MEGA - pDevExt->stJob.nBufSize,
                 pDevExt->nNumOfScts, pDevExt->nClusterSize, 0);
        if (pDevExt->pCCtx == NULL)
        {
            nRet = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        nRet = STATUS_SUCCESS;
    } while (0);

    return nRet;
}

static VOID
_SyncFilterWithTarget(PDEVICE_OBJECT FilterDevice, PDEVICE_OBJECT TargetDevice)
{
    PAGED_CODE();

    FilterDevice->Flags |= TargetDevice->Flags
                           & FILTER_DEVICE_PROPOGATE_FLAGS;
    FilterDevice->Characteristics |= TargetDevice->Characteristics
                                     & FILTER_DEVICE_PROPOGATE_CHARACTERISTICS;
}


static NTSTATUS
_StartDevice(PDEVICE_OBJECT pDevObj, PIRP Irp)
{
    PDEVICE_EXTENSION   pDevExt;
    NTSTATUS            nRet;

    PAGED_CODE();

    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;
    nRet = _ForwardIrpSynchronous(pDevExt, Irp);

    _SyncFilterWithTarget(pDevObj, pDevExt->pTgtDevObj);

    Irp->IoStatus.Status = nRet;

    _InitWriteBufer(pDevExt);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return nRet;
}

static NTSTATUS
_RemoveDevice(PDEVICE_OBJECT pDevObj, PIRP Irp)
{
    NTSTATUS            nRet;
    PDEVICE_EXTENSION   pDevExt;

    PAGED_CODE();

    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    if (pDevExt->nDisabled == 0)
    {
        pDevExt->nDisabled |= FFIRE_DISABLE_REMOVE;
        pDevExt->bTriggerFlush = TRUE;
        KeSetEvent(&pDevExt->evIrpReq, IO_DISK_INCREMENT, TRUE);
        KeWaitForSingleObject(&pDevExt->evFlushed, Executive,
                              KernelMode, FALSE, NULL);
    }
    ExFreePool(pDevExt->pWBMem);

    pDevExt->pWBMem = NULL;

    nRet = _ForwardIrpSynchronous(pDevExt, Irp);

    IoDetachDevice(pDevExt->pTgtDevObj);
    IoDeleteDevice(pDevObj);

    Irp->IoStatus.Status = nRet;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return nRet;
}

static NTSTATUS
_DeviceUsageNotification(PDEVICE_OBJECT pDevObj, PIRP Irp)
{
    PDEVICE_EXTENSION  pDevExt;
    PIO_STACK_LOCATION pIOStk;
    BOOLEAN            bPagable;
    ULONG              nCnt;
    NTSTATUS           nRet;

    pDevExt = pDevObj->DeviceExtension;
    pIOStk  = IoGetCurrentIrpStackLocation(Irp);
    do
    {
        if (pIOStk->Parameters.UsageNotification.Type != DeviceUsageTypePaging)
        {
            nRet = FFIREBypassIrp(pDevObj, Irp);
            break;
        }
        nRet = KeWaitForSingleObject(&pDevExt->evPagingPathCount,
                                       Executive, KernelMode,
                                       FALSE, NULL);
        bPagable = FALSE;
        if ((!pIOStk->Parameters.UsageNotification.InPath) &&
            (pDevExt->nPagingPathCount == 1))
        {
            if ((pDevObj->Flags & DO_POWER_INRUSH)  == 0)
            {
                pDevObj->Flags |= DO_POWER_PAGABLE;
                bPagable        = TRUE;
            }
        }
        nRet = _ForwardIrpSynchronous(pDevExt, Irp);

        if (NT_SUCCESS(nRet))
        {
            IoAdjustPagingPathCount(&pDevExt->nPagingPathCount,
                          pIOStk->Parameters.UsageNotification.InPath);

            if (pIOStk->Parameters.UsageNotification.InPath)
            {
                if (pDevExt->nPagingPathCount == 1)
                {
                    pDevObj->Flags &= ~DO_POWER_PAGABLE;
                }
            }
        }
        else
        {
            if (bPagable == TRUE)
            {
                pDevObj->Flags &= ~DO_POWER_PAGABLE;
                bPagable        = FALSE;
            }
        }

        KeSetEvent(&pDevExt->evPagingPathCount, IO_NO_INCREMENT, FALSE);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    } while (0);

    return nRet;
}

static NTSTATUS
_IrpCompletion(PDEVICE_OBJECT pDevObj, PIRP Irp, PVOID Context)
{
    PKEVENT pEvent = (PKEVENT) Context;

    UNREFERENCED_PARAMETER(pDevObj);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(pEvent, IO_NO_INCREMENT, FALSE);

    return (STATUS_MORE_PROCESSING_REQUIRED);
}

static NTSTATUS
_ForwardIrpSynchronous(PDEVICE_EXTENSION pDevExt, PIRP Irp)
{
    PDEVICE_OBJECT pDevObj;
    KEVENT         evDone;
    NTSTATUS       nRet;

    pDevObj = pDevExt->pDevObj;

    KeInitializeEvent(&evDone, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, _IrpCompletion, &evDone, TRUE, TRUE, TRUE);

    nRet = IoCallDriver(pDevExt->pTgtDevObj, Irp);

    if (nRet == STATUS_PENDING)
    {
        KeWaitForSingleObject(&evDone, Executive, KernelMode, FALSE, NULL);
        nRet = Irp->IoStatus.Status;
    }
    return nRet;
}

static void
_DoFlush(PDEVICE_EXTENSION pDevExt, U32 nCnt)
{
    while (FFBUF_Flush(pDevExt->pCCtx, &pDevExt->stJob))
    {
        _WriteBlk(pDevExt, pDevExt->stJob.nLsn,
                  pDevExt->stJob.nCnt, pDevExt->stJob.pBuf);
        if (--nCnt == 0) break;
    }
}

static BOOLEAN
_DoFlushDeadlined(PDEVICE_EXTENSION pDevExt, U32 nDeadline)
{
    BOOLEAN bWritten = FALSE;

    while (FFBUF_FlushDeadlined(pDevExt->pCCtx, &pDevExt->stJob, nDeadline))
    {
        _WriteBlk(pDevExt, pDevExt->stJob.nLsn,
                  pDevExt->stJob.nCnt, pDevExt->stJob.pBuf);
        bWritten = TRUE;
    }
    return bWritten;
}


/* Block access routines */

static NTSTATUS
_IOCtlBlk(PDEVICE_EXTENSION pDevExt, ULONG nCode,
          PVOID     pIBuf,     ULONG  nIBufSize,
          PVOID pOBuf, PULONG pOBufSize)
{
    ULONG           nOBufSize;
    KEVENT          evEvent;
    PIRP            pIrp;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS        nStatus;

    PAGED_CODE();
    ASSERT(pDevExt != NULL);

    nOBufSize = (pOBufSize) ? (*pOBufSize) : 0;
    KeInitializeEvent(&evEvent, NotificationEvent, FALSE);

    pIrp = IoBuildDeviceIoControlRequest(nCode, pDevExt->pTgtDevObj,
                                         pIBuf, nIBufSize, pOBuf, nOBufSize,
                                         FALSE, &evEvent, &IoStatus);
    if (!pIrp)
    {
        nStatus = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        nStatus = IoCallDriver(pDevExt->pTgtDevObj, pIrp);
        if (nStatus == STATUS_PENDING)
        {
            KeWaitForSingleObject(&evEvent, Executive, KernelMode,
                                  FALSE, NULL);
            nStatus = IoStatus.Status;
        }
        if (pOBufSize)
        {
            *pOBufSize = (ULONG) IoStatus.Information;
        }
    }

    return nStatus;
}

static NTSTATUS
_ReadBlk(PDEVICE_EXTENSION pDevExt,
         ULONG nLsn, ULONG nNumOfSct, PVOID pBuffer)
{
    LARGE_INTEGER   stOff;
    KEVENT          evEvent;
    PIRP            pIrp;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS        nStatus;

    PAGED_CODE();

    ASSERT(pDevExt != NULL);
    ASSERT(pBuffer != NULL);

    KeInitializeEvent(&evEvent, NotificationEvent, FALSE);
    stOff.QuadPart = (LONGLONG)nLsn * pDevExt->nSctSize;
    nNumOfSct     *= pDevExt->nSctSize;

    pIrp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, pDevExt->pTgtDevObj,
                             pBuffer, nNumOfSct, &stOff, &evEvent, &IoStatus);
    if (!pIrp)
    {
        nStatus = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        nStatus = IoCallDriver(pDevExt->pTgtDevObj, pIrp);
        if (nStatus == STATUS_PENDING)
        {
            KeWaitForSingleObject(&evEvent, Executive, KernelMode,
                                  FALSE, NULL);
            nStatus = IoStatus.Status;
        }
    }

    return nStatus;
}

static NTSTATUS
_WriteBlk(PDEVICE_EXTENSION pDevExt,
          ULONG nLsn, ULONG nNumOfSct, PVOID pBuffer)
{
    LARGE_INTEGER   stOff;
    KEVENT          evEvent;
    PIRP            pIrp;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS        nStatus;

    PAGED_CODE();

    ASSERT(pDevExt != NULL);
    ASSERT(pBuffer != NULL);
    stOff.QuadPart = (LONGLONG)nLsn * pDevExt->nSctSize;
    nNumOfSct     *= pDevExt->nSctSize;

    KeInitializeEvent(&evEvent, NotificationEvent, FALSE);

    pIrp = IoBuildSynchronousFsdRequest(IRP_MJ_WRITE, pDevExt->pTgtDevObj,
                             pBuffer, nNumOfSct, &stOff, &evEvent, &IoStatus);
    if (!pIrp)
    {
        nStatus = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        nStatus = IoCallDriver(pDevExt->pTgtDevObj, pIrp);
        if (nStatus == STATUS_PENDING)
        {
            KeWaitForSingleObject(&evEvent, Executive, KernelMode,
                                  FALSE, NULL);
            nStatus = IoStatus.Status;
        }
    }
    return nStatus;
}

static U32
_GetTick(void)
{
    static LARGE_INTEGER lBase;
    static ULONG         ti;
    LARGE_INTEGER        lNow;

    if (lBase.QuadPart == 0)
    {
        KeQueryTickCount(&lBase);
        ti = KeQueryTimeIncrement();
    }
    KeQueryTickCount(&lNow);
    lNow.QuadPart -= lBase.QuadPart;
    lNow.QuadPart *= ti;        /*  0.1us            */
    lNow.QuadPart /= 10000;     /* to 1ms resolution */

    return (U32)lNow.LowPart;
}


/* Registry related functions */
static NTSTATUS
_WriteRegParam(PCWSTR pKey, ULONG nValue)
{
    UNICODE_STRING    strParmPath;
    UNICODE_STRING    strKey;
    OBJECT_ATTRIBUTES stObjAttr;
    HANDLE            hKeyHandle;
    NTSTATUS          nStatus;

    RtlInitUnicodeString(&strParmPath, FFIRE_REGPATH);
    InitializeObjectAttributes(&stObjAttr, &strParmPath,
                       OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    nStatus = ZwOpenKey(&hKeyHandle, KEY_READ | KEY_WRITE, &stObjAttr);
    if (NT_SUCCESS (nStatus))
    {
        RtlInitUnicodeString(&strKey, pKey);
        nStatus = ZwSetValueKey(hKeyHandle, &strKey, REG_DWORD, REG_DWORD,
                                &nValue, sizeof(ULONG));

        ZwClose(hKeyHandle);
    }
    return nStatus;
}

static NTSTATUS
_ReadRegParam(PWSTR pKey, PULONG pValue)
{
    RTL_QUERY_REGISTRY_TABLE aQuary[2];
    UNICODE_STRING           strParmPath;

    RtlZeroMemory(&aQuary[0], sizeof(aQuary));
    RtlInitUnicodeString(&strParmPath, FFIRE_REGPATH);

    aQuary[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED;
    aQuary[0].Name  = pKey;
    aQuary[0].EntryContext = pValue;

    return RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, strParmPath.Buffer,
                                  &aQuary[0], NULL, NULL);
}


static BOOLEAN
_HandleReadIrp(PDEVICE_EXTENSION pDevExt,    ULONG nLsn,
               ULONG             nNumOfScts, PCHAR pSysBuf)
{
    BOOLEAN bForward;
    int  j;

    j = FFBUF_CheckHitMiss(pDevExt->pCCtx, nLsn, nNumOfScts);
    if (j == nNumOfScts)
    {
        bForward = TRUE;
    }
    else
    {
        while (nNumOfScts > 0)
        {
            if (j > 0)
            {
                _ReadBlk(pDevExt, nLsn, j, pSysBuf);
            }
            else
            {
                j *= -1;
                FFBUF_Get(pDevExt->pCCtx, nLsn, j, pSysBuf);
            }
            nNumOfScts -= j;
            if (nNumOfScts == 0) break;

            nLsn    += j;
            pSysBuf += j * pDevExt->nSctSize;
            j = FFBUF_CheckHitMiss(pDevExt->pCCtx, nLsn, nNumOfScts);
        }
        bForward = FALSE;
    }
    return bForward;
}

static BOOLEAN
_HandleWriteIrp(PDEVICE_EXTENSION pDevExt,    ULONG nLsn,
                ULONG             nNumOfScts, PCHAR pSysBuf)
{
    ULONG nWritten;

    while (nNumOfScts)
    {
        nWritten = FFBUF_Put(pDevExt->pCCtx, nLsn, nNumOfScts, pSysBuf,
                             &pDevExt->stJob, pDevExt->nCurTime);
        if (pDevExt->stJob.nCnt)
        {
            _WriteBlk(pDevExt, nLsn, pDevExt->stJob.nCnt, pDevExt->stJob.pBuf);
        }
        nLsn       += nWritten;
        nNumOfScts -= nWritten;
        pSysBuf    += nWritten * pDevExt->nSctSize;
    }

    return FALSE;
}

static BOOLEAN
_HandleRWIrp(PIRP pIrp, PIO_STACK_LOCATION pIOStk, PDEVICE_EXTENSION pDevExt)
{
    ULONG   nLsn;
    ULONG   nNumOfScts;
    PUCHAR  pSysBuf;
    BOOLEAN bForward;
    int     j;


    nLsn = (ULONG)(pIOStk->Parameters.Read.ByteOffset.QuadPart /
                   pDevExt->nSctSize);
    nNumOfScts = (ULONG)(pIOStk->Parameters.Read.Length) / pDevExt->nSctSize;

    pSysBuf = (PUCHAR)
        MmGetSystemAddressForMdlSafe(pIrp->MdlAddress, NormalPagePriority);

    if (pDevExt->nDisabled)
    {
        bForward = TRUE;
    }
    else if (pIOStk->MajorFunction == IRP_MJ_READ)
    {
        bForward = _HandleReadIrp(pDevExt, nLsn, nNumOfScts, pSysBuf);
    }
    else
    {
        bForward = _HandleWriteIrp(pDevExt, nLsn, nNumOfScts, pSysBuf);
    }

    return bForward;
}

static BOOLEAN
_HandleDeadLine(PDEVICE_EXTENSION pDevExt)
{
    BOOLEAN bWritten = FALSE;

    if (((pDevExt->nOption & FFIRE_OPTION_NODEADLINE) == 0) &&
        (pDevExt->nCurTime  > FFIRE_DEADLINE))
    {
        bWritten = _DoFlushDeadlined(pDevExt,
                                     pDevExt->nCurTime - FFIRE_DEADLINE);
    }
    return bWritten;
}

static VOID
_IrpHandlingThread(PVOID Context)
{
    PVOID               aWaitObjs[2];

    PDEVICE_OBJECT      pDevObj;
    PDEVICE_EXTENSION   pDevExt;
    PLIST_ENTRY         pListEntry;
    PIRP                pIrp;
    PIO_STACK_LOCATION  pIOStk;
    LARGE_INTEGER       stDueTime;
    U32                 nFlushSpeed;
    U32                 nTickCnt;
    NTSTATUS            nRet;


    pDevObj = (PDEVICE_OBJECT) Context;
    pDevExt = (PDEVICE_EXTENSION) pDevObj->DeviceExtension;

    stDueTime.QuadPart = -10000 * FFIRE_TIMER_TICK; /* 200ms */
    KeSetTimerEx(&pDevExt->tiTickTimer, stDueTime, FFIRE_TIMER_TICK, NULL);

    aWaitObjs[0] = &pDevExt->evIrpReq;
    aWaitObjs[1] = &pDevExt->tiTickTimer;

    nTickCnt = 0;

    while (1)
    {
        nRet = KeWaitForMultipleObjects(2, aWaitObjs, WaitAny,
                                        Executive,
                                        KernelMode,
                                        FALSE,
                                        (PLARGE_INTEGER) NULL,
                                        (PKWAIT_BLOCK) NULL);
        /* Thread Terminate */
        if (pDevExt->bTerminateThread)
        {
            KeCancelTimer(&pDevExt->tiTickTimer);
            _DoFlush(pDevExt, 0);
            PsTerminateSystemThread(STATUS_SUCCESS);
        }

        pDevExt->nCurTime = _GetTick();

        /* Irp arrives event */
        if (nRet == STATUS_WAIT_0)
        {
            while (pListEntry =
                    ExInterlockedRemoveHeadList(&pDevExt->listIrpHead,
                                                &pDevExt->lockIrpList))
            {
                pDevExt->nCurTime = _GetTick();

                nTickCnt    = 0;
                nFlushSpeed = 1;

                pIrp = CONTAINING_RECORD(pListEntry,
                                         IRP, Tail.Overlay.ListEntry);
                pIOStk = IoGetCurrentIrpStackLocation(pIrp);
                switch (pIOStk->MajorFunction)
                {
                case IRP_MJ_READ:
                case IRP_MJ_WRITE:
                    if (_HandleRWIrp(pIrp, pIOStk, pDevExt))
                    {
                        _ForwardIrpSynchronous(pDevExt, pIrp);
                    }
                    break;

                case IRP_MJ_SHUTDOWN:
                    _DoFlush(pDevExt, 0);
                    _ForwardIrpSynchronous(pDevExt, pIrp);
                    break;

                case IRP_MJ_FLUSH_BUFFERS:
                    if ((pDevExt->nOption & FFIRE_OPTION_NOFLUSH) == 0)
                    {
                        _DoFlush(pDevExt, 0);
                    }
                    _ForwardIrpSynchronous(pDevExt, pIrp);
                    break;

                default:
                    pIrp->IoStatus.Status = STATUS_DRIVER_INTERNAL_ERROR;
                    break;
                }
                IoCompleteRequest(pIrp,
                    (CCHAR)(NT_SUCCESS(pIrp->IoStatus.Status) ?
                                    IO_DISK_INCREMENT : IO_NO_INCREMENT));

                _HandleDeadLine(pDevExt);
            }
        }
        /* Timer event */
        else
        {
            if (!_HandleDeadLine(pDevExt))
            {
                if (++nTickCnt >= FFIRE_TIMER_TICKCNT)
                {
                    _DoFlush(pDevExt, nFlushSpeed);
                    nFlushSpeed <<= 1;
                    if (pDevExt->nDisabled) pDevExt->bBypassOn = TRUE;
                }
            }
        }

        if (pDevExt->bTriggerFlush)
        {
            _DoFlush(pDevExt, 0);
            pDevExt->bTriggerFlush = FALSE;
            KeSetEvent(&pDevExt->evFlushed, (KPRIORITY)0, FALSE);
        }
    }
}