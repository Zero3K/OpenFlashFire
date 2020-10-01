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

#define __FFIRE_MONITOR__

#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include "ffbuf.h"
#include "offire.h"

#define MAX_DEVNAMELEN      (20)

struct
{
    char*   pCmd;
    char*   pHelp;
    ULONG   nCode;
} gaCmdList[] = 
{
    {"-enable",   "Enable OpenFlashFire",  FFIRE_IOCTL_ENABLE  },
    {"-disable",  "Disable OpenFlashFire", FFIRE_IOCTL_DISABLE },
    {"-flush",    "Toggle Flush Mode",     FFIRE_IOCTL_FLUSH   },
    {"-deadline", "Toggle Deadline Mode",  FFIRE_IOCTL_DEADLINE},
    {NULL, NULL, 0},
};


static int   
_GetInfo(HANDLE hDevHandle, FFIREINFO* pInfo)
{
    ULONG  nDummy;
    int    nRet = 0;

    if (hDevHandle != INVALID_HANDLE_VALUE)
    {
        nRet = DeviceIoControl(hDevHandle,
                               FFIRE_IOCTL_INFO,
                               NULL, 0,
                               pInfo, sizeof(FFIREINFO),
                               &nDummy,
                               (struct _OVERLAPPED *)NULL);
    }
    return nRet;
}

static int
_SendCmd(HANDLE hDevHandle, ULONG nCmd)
{
    ULONG  nDummy;
    int    nRet;

    nRet = DeviceIoControl(hDevHandle,
                           nCmd,
                           NULL, 0,
                           NULL, 0,
                           &nDummy,
                           (struct _OVERLAPPED *)NULL);
    return nRet;
}

static HANDLE
_FindAndOpenDevice(FFIREINFO* pInfo)
{
    char      aDevName[MAX_DEVNAMELEN];
    HANDLE    hDevHandle;
    int       nDevNum;

    for (nDevNum = 0; ; nDevNum++)
    {
        sprintf(aDevName, "\\\\.\\PhysicalDrive%d", nDevNum);

        hDevHandle = CreateFile(aDevName,
                           GENERIC_READ    | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           0,
                           OPEN_EXISTING,
                           0,
                           0);
        if (hDevHandle == INVALID_HANDLE_VALUE) break;
        if (_GetInfo(hDevHandle, pInfo)) break;
    }
    return hDevHandle;
}

static void
_PrintInfo(FFIREINFO* pInfo)
{
    FFBCTX* pFFCtx;
    ULONG   nBufUsage;

    pFFCtx = &pInfo->stCtx;

    printf("-=-=- OpenFlashFire on Disk %d -=-=-\n", pInfo->nDiskNum);
    printf("* Version               : %05X\n", pInfo->nVersion);
    printf("* Status                : %s\n", 
           (pInfo->nDisabled) ? "Disabled" : "Enabled");
    printf("* Flush                 : %s\n", 
           (pInfo->nOption & FFIRE_OPTION_NOFLUSH) ? "Off" : "On");
    printf("* Deadline              : ");
    if (pInfo->nOption & FFIRE_OPTION_NODEADLINE) printf("Off\n");
    else printf("%d seconds\n", pInfo->nDeadline);
    
    nBufUsage = (pFFCtx->nNumOfBuf - pFFCtx->nFreeBufCnt) * 100 / 
                pFFCtx->nNumOfBuf;

    printf("* Buffer Size           : %d Mbytes\n",  
                                      pFFCtx->nBufSize / FFIRE_1MEGA);
    printf("* Total Buffers         : %d\n", pFFCtx->nNumOfBuf);
    printf("* Free  Buffers         : %d\n", pFFCtx->nFreeBufCnt);
    printf("* Buffer Usage          : %3d %%\n", nBufUsage);
}

static void
_PrintUsage(void)
{
    int i;

    printf("-=-=- Help -=-=-\n");
    printf("Syntax: offmon.exe <commands>\n");
    for (i = 0; gaCmdList[i].pCmd; i++)
    {
        printf("\t%s : %s\n", gaCmdList[i].pCmd, gaCmdList[i].pHelp);
    }
}

int __cdecl 
main(int nArgc, char* pArgv[])
{
    FFIREINFO stInfo;
    HANDLE    hDevHandle;
    int       nRet;
    int       i, j;
    int       nProcessedCmd;

    printf("# OpenFlashFire Monitor Copyright(c) 2009-2010 Hyojun Kim\n");
    hDevHandle = _FindAndOpenDevice(&stInfo);
    if (hDevHandle != INVALID_HANDLE_VALUE) 
    {
        nProcessedCmd = 0;
        for (i = 1; i < nArgc; i++)
        {
            for (j = 0; gaCmdList[j].pCmd; j++)
            {
                if (strcmp(pArgv[i], gaCmdList[j].pCmd) == 0) 
                {
                    nProcessedCmd++;
                    _SendCmd(hDevHandle, gaCmdList[j].nCode);
                }
            }
        }
        
        _GetInfo(hDevHandle, &stInfo);
        _PrintInfo(&stInfo);

        if (nProcessedCmd == 0)
        {
            _PrintUsage();
        }


        CloseHandle(hDevHandle);
        nRet = 0;
    }
    else
    {
        printf("\tOpenFlashFire is not found\n");
        nRet = -1;
    }
    return nRet;
}
