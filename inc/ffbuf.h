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

#ifndef __FFBUF_H__
#define __FFBUF_H__

typedef unsigned char   U08;
typedef unsigned short  U16;
typedef unsigned int    U32;

typedef struct
{
    /* Input */
    U08*    pBuf;
    U32     nBufSize;

    /* Output */
    U32     nCnt;
    U32     nLsn;
} WJOB;

void* FFBUF_Init(U08* pWBuf, U32 nBufSize, U32 nNumOfSct, U32 nSctPerClt, U32 nOpt);
U32   FFBUF_Put(void* pHndl, U32 nLsn, U32 nCnt, U08* pBuf, WJOB* pJob, U32 nTimeTick);
U32   FFBUF_Get(void* pHndl, U32 nLsn, U32 nCnt, U08* pBuf);
int   FFBUF_Check(void* pHndl, U32 nLsn, U32 nCnt);
int	  FFBUF_CheckHitMiss(void* pHndl, U32 nLsn, U32 nCnt);
U32   FFBUF_Del(void* pHndl, U32 nLsn, U32 nCnt);
U32   FFBUF_Flush(void* pHndl, WJOB* pJob);
U32   FFBUF_FlushDeadlined(void* pHndl, WJOB* pJob, U32 nDeadline);

/* Internal purpose */

#define FFB_NILL32          (0xffffffff)
#define FFB_SCTSIZE         (512)
#define FFB_L2SIZE          (FFB_SCTSIZE / sizeof(U32))
#define FFB_BUFADDR(c, i)   ((c->pBufBase) + (i * FFB_SCTSIZE))
#define FFB_MINFREE_CLT     (3)
#define FFB_MINFREE_L1      (2)
#define FFB_MINFREE_L2      (1)
#define FFB_CFG_SEQTH       (70)

#define FFB_ASSERT(f)

/*
#define FFB_ASSERT(f)                             \
    {                                             \
        if (!(f))                                 \
        {                                         \
            printf("ASSERT : %s, line : %d \n",   \
                    __FILE__,  __LINE__);         \
            while (1);                            \
        }                                         \
    }
*/

typedef struct _clthdr
{
    /* for LRU list */
    struct _clthdr* pPrev;
    struct _clthdr* pNext;

    /* for Deadline Queue */
    U32             nTimeStamp;
    struct _clthdr* pNewer;
    struct _clthdr* pOlder;

    /* Number of Sector in the cluster */
    U32             nCltNum;
    U32             nSctCnt;
    U32*            pL1Tbl;
    U08*            pL1Cnt;
} CLTHDR;

typedef struct
{
    U32             nBufSize;
    U32             nNumOfSct;

    U32             nSctPerClt;
    U32             nOpt;

    U32             nSeqTH;
    U32             nNumOfClt;

    U32             nNumOfBuf;
    U32             nL1Size;
    
    U32             nFreeBufCnt;
    U32             nFreeBufHead;

    U32             nNumOfCltInBuf;

    U32*            pCltTbl;
    U08*            pBufBase;

    CLTHDR*         pCLHead;
    CLTHDR*         pVictimClt;
    
    CLTHDR*         pCLOldest;
} FFBCTX;

typedef struct
{
    U32             nVersion;
    U32             nDiskNum;
    U32             nOption;
    U32             nDisabled;
    U32             nDeadline;

    FFBCTX          stCtx;
} FFIREINFO;

#endif /* __FFBUF_H__ */