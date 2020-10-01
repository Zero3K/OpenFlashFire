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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffbuf.h"

static U32     _GetFreeBuf(FFBCTX* pCtx);
static void    _PutFreeBuf(FFBCTX* pCtx, U32 nBIdx);
static void    _SelectVictim(FFBCTX* pCtx, U32 nHotLcn, U32 nDeadline);
static void    _MakeSpace(FFBCTX* pCtx, U32 nHotLcn, WJOB* pJob, U32 nDeadline);
static CLTHDR* _InitCltHdr(FFBCTX* pCtx, U32 nLcn, U32 nTimeTick);
static void    _InsertToLRUList(FFBCTX* pCtx, CLTHDR* pClt);
static void    _RemoveFromLRUList(FFBCTX* pCtx, CLTHDR* pClt);
static void    _InsertToDLQ(FFBCTX* pCtx, CLTHDR* pClt);
static void    _RemoveFromCLQ(FFBCTX* pCtx, CLTHDR* pClt);
static void    _DeQueueAndFreeClt(FFBCTX* pCtx, CLTHDR* pClt);
static void    _RollLeft(FFBCTX* pCtx);

/*****************************************************************************
 *                           Interface Functions 
 *****************************************************************************/

void* 
FFBUF_Init(U08* pWBuf, U32 nBufSize, U32 nNumOfSct, U32 nSctPerClt, U32 nOpt)
{
    FFBCTX* pCtx;
    void*   pRet;
    U32     nSize;
    U32     nIdx;
    
    nSctPerClt /= FFB_L2SIZE;
    nSctPerClt *= FFB_L2SIZE;

    pRet = NULL;
    do
    {
        if (nBufSize < sizeof(FFBCTX)) break;
        pCtx = (FFBCTX*)pWBuf;
        memset(pCtx, 0, sizeof(FFBCTX));

        pCtx->nBufSize   = nBufSize;
        pCtx->nNumOfSct  = nNumOfSct;
        pCtx->nSctPerClt = nSctPerClt;
        pCtx->nOpt       = nOpt;
        pCtx->nL1Size    = nSctPerClt / FFB_L2SIZE;
        pCtx->nSeqTH     = nSctPerClt * FFB_CFG_SEQTH / 100;

        pCtx->nNumOfClt  = (nNumOfSct + nSctPerClt - 1) / nSctPerClt;
        nBufSize -= sizeof(FFBCTX);
        pWBuf    += sizeof(FFBCTX);

        pCtx->pCltTbl    = (U32*)pWBuf;
        nSize            = pCtx->nNumOfClt * sizeof(U32);
        if (nBufSize < nSize) break;
        memset(pCtx->pCltTbl, FFB_NILL32, nSize);
        nBufSize -= nSize;
        pWBuf    += nSize;

        pCtx->nNumOfBuf = nBufSize / FFB_SCTSIZE;
        if (pCtx->nNumOfBuf < (nSctPerClt * 2)) break;
        pCtx->pBufBase  = pWBuf;
        
        for (nIdx = 0; nIdx < (pCtx->nNumOfBuf - 1); nIdx++)
        {
            *((U32*)(pCtx->pBufBase + nIdx * FFB_SCTSIZE)) = nIdx + 1;
        }
        *((U32*)(pCtx->pBufBase + nIdx * FFB_SCTSIZE)) = FFB_NILL32;
        pCtx->nFreeBufCnt  = pCtx->nNumOfBuf;
        pCtx->nFreeBufHead = 0;
        pRet = pCtx;
    } while (0);

    return pRet;
}

U32   
FFBUF_Put(void* pHndl, U32 nLsn, U32 nCnt, U08* pBuf, WJOB* pJob, U32 nTimeTick)
{
    FFBCTX* pCtx = (FFBCTX*)pHndl;
    CLTHDR* pClt;
    U32*    pL2Tbl;
    U32     nRet;
    U32     nOffset;
    U32     nLcn;
    U32     nL1;
    U32     nL2;
    U32     nL1Idx;
    U32     nL2Idx;
    U32     nClt;

    nRet       = 0;
    pJob->nCnt = 0;
    pJob->nLsn = FFB_NILL32;
    while (nCnt > 0)
    {
        nLcn = nLsn / pCtx->nSctPerClt;
        nClt = pCtx->pCltTbl[nLcn];

        if (nClt == FFB_NILL32)
        {
            if (pCtx->nFreeBufCnt < FFB_MINFREE_CLT) 
            {
                if (pJob->nCnt == 0) _MakeSpace(pCtx, nLcn, pJob, 0);
            }
            if (pCtx->nFreeBufCnt < FFB_MINFREE_CLT) break;

            nClt = _GetFreeBuf(pCtx);
            FFB_ASSERT(nClt < pCtx->nNumOfBuf);
            pCtx->pCltTbl[nLcn] = nClt;

            pClt = _InitCltHdr(pCtx, nLcn, nTimeTick);
        }
        else
        {
            pClt = (CLTHDR*)FFB_BUFADDR(pCtx, nClt);

            /* LRU */
            if (pClt != pCtx->pCLHead)
            {
                _RemoveFromLRUList(pCtx, pClt);
                _InsertToLRUList(pCtx, pClt);
            }
            if (pClt == pCtx->pVictimClt)
            {
                pCtx->pVictimClt = NULL;
            }
        }
        nOffset = nLsn % pCtx->nSctPerClt;
        nL1Idx  = nOffset / FFB_L2SIZE;
        nL1     = pClt->pL1Tbl[nL1Idx];
        if (nL1 == FFB_NILL32)
        {
            if (pCtx->nFreeBufCnt < FFB_MINFREE_L1)
            {
                if (pJob->nCnt == 0) _MakeSpace(pCtx, nLcn, pJob, 0);
            }
            if (pCtx->nFreeBufCnt < FFB_MINFREE_L1) break;

            nL1 = _GetFreeBuf(pCtx);
            FFB_ASSERT(nL1 < pCtx->nNumOfBuf);
            pClt->pL1Tbl[nL1Idx] = nL1;

            pL2Tbl = (U32*)FFB_BUFADDR(pCtx, nL1);
            memset(pL2Tbl, FFB_NILL32, FFB_SCTSIZE);
        }
        else
        {
            pL2Tbl = (U32*)FFB_BUFADDR(pCtx, nL1);
        }
        nL2Idx = nOffset % FFB_L2SIZE;
        do
        {
            nL2 = pL2Tbl[nL2Idx];
            if (nL2 == FFB_NILL32)
            {
                if (pCtx->nFreeBufCnt < FFB_MINFREE_L2)
                {
                    if (pJob->nCnt == 0) _MakeSpace(pCtx, nLcn, pJob, 0);
                }
                if (pCtx->nFreeBufCnt < FFB_MINFREE_L2) 
                {
                    return nRet;
                }
                nL2 = _GetFreeBuf(pCtx);
                FFB_ASSERT(nL2 < pCtx->nNumOfBuf);
                pL2Tbl[nL2Idx] = nL2;
                pClt->pL1Cnt[nL1Idx] += 1;
                pClt->nSctCnt++;

                FFB_ASSERT(pClt->pL1Cnt[nL1Idx] <= FFB_L2SIZE);
                FFB_ASSERT(pClt->nSctCnt        <= pCtx->nSctPerClt);
            }
            memcpy(FFB_BUFADDR(pCtx, nL2), pBuf, FFB_SCTSIZE);

            pBuf += FFB_SCTSIZE;
            nLsn++;
            nRet++;
            nCnt--;
            nL2Idx++;
        } while ((nL2Idx < FFB_L2SIZE) && (nCnt > 0));

        if (((nLsn % pCtx->nSctPerClt) == 0) &&  /* Cluster Boundary Crossing    */
            (pClt->nSctCnt >= pCtx->nSeqTH))     /* Number of Sectors in Cluster */
        {
            _RollLeft(pCtx);
        }
    }

    return nRet;
}

U32
FFBUF_Get(void* pHndl, U32 nLsn, U32 nCnt, U08* pBuf)
{
    FFBCTX* pCtx = (FFBCTX*)pHndl;
    CLTHDR* pClt;
    U32*    pL2Tbl;
    U32     nLcn;
    U32     nOffset;
    U32     nOldLsn;
    U32     nEndLsn;
    U32     nL1;
    U32     nL2;
    U32     nL1Idx;
    U32     nL2Idx;
    U32     nClt;
    U32     nRet;

    nRet    = 0;
    nEndLsn = nLsn + nCnt;
    nOldLsn = nLsn;
    while (1)
    {
        pBuf += (nLsn - nOldLsn) * FFB_SCTSIZE;
        if (nLsn >= nEndLsn) break;
        nOldLsn = nLsn;

        nLcn = nLsn / pCtx->nSctPerClt;
        nClt = pCtx->pCltTbl[nLcn];
        if (nClt == FFB_NILL32)
        {
            nLsn = (nLcn + 1) * pCtx->nSctPerClt;
            continue;
        }
        pClt = (CLTHDR*)FFB_BUFADDR(pCtx, nClt);

        nOffset = nLsn % pCtx->nSctPerClt;
        nL1Idx  = nOffset / FFB_L2SIZE;
        nL1     = pClt->pL1Tbl[nL1Idx];
        if (nL1 == FFB_NILL32)
        {
            nLsn = (nLcn * pCtx->nSctPerClt) + (nL1Idx + 1) * FFB_L2SIZE;
            continue;
        }

        pL2Tbl = (U32*)FFB_BUFADDR(pCtx, nL1);
        nL2Idx = nOffset % FFB_L2SIZE;
        do
        {
            nL2 = pL2Tbl[nL2Idx];
            if (nL2 != FFB_NILL32) 
            {
                memcpy(pBuf, FFB_BUFADDR(pCtx, nL2), FFB_SCTSIZE);
                nRet++;
            }
            nL2Idx++;
            nLsn++;
            pBuf += FFB_SCTSIZE;
        } while ((nL2Idx < FFB_L2SIZE) && 
                 (nLsn   < nEndLsn   ));
        nOldLsn = nLsn;
    }
    return nRet;
}

int
FFBUF_CheckHitMiss(void* pHndl, U32 nLsn, U32 nCnt)
{
    FFBCTX* pCtx = (FFBCTX*)pHndl;
    CLTHDR* pClt;
    U32*    pL2Tbl;
    U32     nLcn;
    U32     nOffset;
    U32     nEndLsn;
	U32     nNextLsn;
    U32     nL1;
    U32     nL2;
    U32     nL1Idx;
    U32     nL2Idx;
    U32     nClt;
	int     nHitMiss;		/* Plus: Misses, Minus: Hits */

	nHitMiss = 0;
    nEndLsn  = nLsn + nCnt;
    while (nLsn < nEndLsn)
    {
		nLcn = nLsn / pCtx->nSctPerClt;
		nClt = pCtx->pCltTbl[nLcn];
		if (nClt == FFB_NILL32)
		{
			nNextLsn = (nLcn + 1) * pCtx->nSctPerClt;
			if (nNextLsn > nEndLsn) nNextLsn = nEndLsn;

			if (nHitMiss < 0) return nHitMiss;

			nHitMiss += nNextLsn - nLsn;
			nLsn      = nNextLsn;
			continue;
		}
		pClt = (CLTHDR*)FFB_BUFADDR(pCtx, nClt);

		nOffset = nLsn % pCtx->nSctPerClt;
		nL1Idx  = nOffset / FFB_L2SIZE;
		nL1     = pClt->pL1Tbl[nL1Idx];
		if (nL1 == FFB_NILL32)
		{
			nNextLsn = (nLcn * pCtx->nSctPerClt) + (nL1Idx + 1) * FFB_L2SIZE;
			if (nNextLsn > nEndLsn) nNextLsn = nEndLsn;

			if (nHitMiss < 0) return nHitMiss;

			nHitMiss += nNextLsn - nLsn;
			nLsn      = nNextLsn;
			continue;
		}

		pL2Tbl = (U32*)FFB_BUFADDR(pCtx, nL1);
		nL2Idx = nOffset % FFB_L2SIZE;
		do
		{
			nL2 = pL2Tbl[nL2Idx];
			if (nL2 != FFB_NILL32) 
			{
				if (nHitMiss > 0) return nHitMiss;
				nHitMiss--;
			}
			else
			{
				if (nHitMiss < 0) return nHitMiss;
				nHitMiss++;
			}
			nL2Idx++;
			nLsn++;
		} while ((nL2Idx < FFB_L2SIZE) && 
				 (nLsn   < nEndLsn   ));
    }
    return nHitMiss;
}

int
FFBUF_Check(void* pHndl, U32 nLsn, U32 nCnt)
{
    FFBCTX* pCtx = (FFBCTX*)pHndl;
    CLTHDR* pClt;
    U32*    pL2Tbl;
    U32     nLcn;
    U32     nOffset;
    U32     nEndLsn;
    U32     nL1;
    U32     nL2;
    U32     nL1Idx;
    U32     nL2Idx;
    U32     nClt;

    nEndLsn = nLsn + nCnt;
    while (nLsn < nEndLsn)
    {
        nLcn = nLsn / pCtx->nSctPerClt;
        nClt = pCtx->pCltTbl[nLcn];
        if (nClt == FFB_NILL32)
        {
            nLsn = (nLcn + 1) * pCtx->nSctPerClt;
            continue;
        }
        pClt = (CLTHDR*)FFB_BUFADDR(pCtx, nClt);

        nOffset = nLsn % pCtx->nSctPerClt;
        nL1Idx  = nOffset / FFB_L2SIZE;
        nL1     = pClt->pL1Tbl[nL1Idx];
        if (nL1 == FFB_NILL32)
        {
            nLsn = (nLcn * pCtx->nSctPerClt) + (nL1Idx + 1) * FFB_L2SIZE;
            continue;
        }

        pL2Tbl = (U32*)FFB_BUFADDR(pCtx, nL1);
        nL2Idx = nOffset % FFB_L2SIZE;
        do
        {
            nL2 = pL2Tbl[nL2Idx];
            if (nL2 != FFB_NILL32) return 1;
            nL2Idx++;
            nLsn++;
        } while ((nL2Idx < FFB_L2SIZE) && 
                 (nLsn   < nEndLsn   ));
    }
    return 0;
}

U32   
FFBUF_Del(void* pHndl, U32 nLsn, U32 nCnt)
{
    FFBCTX* pCtx = (FFBCTX*)pHndl;
    CLTHDR* pClt;
    U32*    pL2Tbl;
    U32     nLcn;
    U32     nOffset;
    U32     nEndLsn;
    U32     nL1;
    U32     nL2;
    U32     nL1Idx;
    U32     nL2Idx;
    U32     nClt;
    U32     nRet;

    nRet    = 0;
    nEndLsn = nLsn + nCnt;
    while (nLsn < nEndLsn)
    {
        nLcn = nLsn / pCtx->nSctPerClt;
        nClt = pCtx->pCltTbl[nLcn];
        if (nClt == FFB_NILL32)
        {
            nLsn = (nLcn + 1) * pCtx->nSctPerClt;
            continue;
        }
        pClt = (CLTHDR*)FFB_BUFADDR(pCtx, nClt);

        nOffset = nLsn % pCtx->nSctPerClt;
        nL1Idx  = nOffset / FFB_L2SIZE;
        nL1     = pClt->pL1Tbl[nL1Idx];
        if (nL1 == FFB_NILL32)
        {
            nLsn = (nLcn * pCtx->nSctPerClt) + (nL1Idx + 1) * FFB_L2SIZE;
            continue;
        }

        pL2Tbl = (U32*)FFB_BUFADDR(pCtx, nL1);
        nL2Idx = nOffset % FFB_L2SIZE;
        do
        {
            nL2 = pL2Tbl[nL2Idx];
            if (nL2 != FFB_NILL32) 
            {
                _PutFreeBuf(pCtx, nL2);

                FFB_ASSERT(pClt->pL1Cnt[nL1Idx] > 0);
                FFB_ASSERT(pClt->nSctCnt        > 0);

                pL2Tbl[nL2Idx] = FFB_NILL32;

                pClt->pL1Cnt[nL1Idx] -= 1;
                pClt->nSctCnt--;
            }
            nL2Idx++;
            nLsn++;
        } while ((nL2Idx < FFB_L2SIZE) && 
                 (nLsn   < nEndLsn   ));
        if (pClt->pL1Cnt[nL1Idx] == 0)
        {
            _PutFreeBuf(pCtx, nL1);
            pClt->pL1Tbl[nL1Idx] = FFB_NILL32;
        }
        if (pClt->nSctCnt == 0)
        {
            _DeQueueAndFreeClt(pCtx, pClt);
        }
    }
    return nRet;
}

U32
FFBUF_Flush(void* pHndl, WJOB* pJob)
{
    FFBCTX* pCtx = (FFBCTX*)pHndl;
    U32     nRet;

    nRet = 0;
    if (pCtx->nFreeBufCnt < pCtx->nNumOfBuf)
    {
        pJob->nCnt = 0;
        pJob->nLsn = FFB_NILL32;
        _MakeSpace(pCtx, FFB_NILL32, pJob, 0);
        nRet = pJob->nCnt;
    }
    return nRet;
}

U32
FFBUF_FlushDeadlined(void* pHndl, WJOB* pJob, U32 nDeadline)
{
    FFBCTX* pCtx = (FFBCTX*)pHndl;
    U32     nRet;

    nRet = 0;

    if ((pCtx->pCLOldest) && (pCtx->pCLOldest->nTimeStamp < nDeadline))
    {
        pJob->nCnt = 0;
        pJob->nLsn = FFB_NILL32;
        _MakeSpace(pCtx, FFB_NILL32, pJob, nDeadline);
        nRet = pJob->nCnt;
    }
    return nRet;
}


/*****************************************************************************
 *                           Internal Functions 
 *****************************************************************************/

static void
_SelectVictim(FFBCTX* pCtx, U32 nHotLcn, U32 nDeadline)
{
    if ((pCtx->pVictimClt == NULL) || (pCtx->pVictimClt->nCltNum == nHotLcn))
    {
        FFB_ASSERT(pCtx->pCLHead);
        
        FFB_ASSERT(pCtx->pCLOldest);
        if ((pCtx->pCLOldest->nTimeStamp <  nDeadline) &&
            (pCtx->pCLOldest->nCltNum    != nHotLcn))
        {
            pCtx->pVictimClt = pCtx->pCLOldest;
        }
        else
        {
            /* LRU selection */
            pCtx->pVictimClt = pCtx->pCLHead->pPrev;

            FFB_ASSERT(pCtx->pVictimClt);
            if (pCtx->pVictimClt->nCltNum == nHotLcn) 
            {
                pCtx->pVictimClt = pCtx->pVictimClt->pPrev;
            }
        }

        FFB_ASSERT(pCtx->pVictimClt);
    }
}

static void
_MakeSpace(FFBCTX* pCtx, U32 nHotLcn, WJOB* pJob, U32 nDeadline)
{
    CLTHDR* pClt;
    U32*    pL2Tbl;
    U08*    pJobBuf;
    U32     nJobSctCnt;
    U32     nL1Idx;
    U32     nL2Idx;
    U32     nL1;
    U32     nL2;

    FFB_ASSERT(pJob);
    FFB_ASSERT(pCtx);
    FFB_ASSERT(pJob->nCnt == 0);

    _SelectVictim(pCtx, nHotLcn, nDeadline);

    nJobSctCnt = pJob->nBufSize / FFB_SCTSIZE;
    pJobBuf    = pJob->pBuf;

    FFB_ASSERT(nJobSctCnt > 0);
    FFB_ASSERT(pJobBuf);

    pClt = pCtx->pVictimClt;
    for (nL1Idx = 0; nL1Idx < pCtx->nL1Size; nL1Idx++)
    {
        nL1 = pClt->pL1Tbl[nL1Idx];
        if (nL1 == FFB_NILL32)
        {
            if (pJob->nLsn != FFB_NILL32) break;
        }
        else 
        {
            pL2Tbl = (U32*)FFB_BUFADDR(pCtx, nL1);
            for (nL2Idx = 0; nL2Idx < FFB_L2SIZE; nL2Idx++, pL2Tbl++)
            {
                nL2 = *pL2Tbl;
                if (pJob->nLsn == FFB_NILL32)
                {
                    if (nL2 != FFB_NILL32)
                    {
                        pJob->nLsn = (pClt->nCltNum * pCtx->nSctPerClt) + 
                                     (nL1Idx * FFB_L2SIZE) + nL2Idx;
                        pJob->nCnt = 1;
                    }
                }
                else
                {
                    if (nL2 == FFB_NILL32) break;
                    pJob->nCnt++;
                }
                if (nL2 != FFB_NILL32)
                {
                    memcpy(pJobBuf, FFB_BUFADDR(pCtx, nL2), FFB_SCTSIZE);
                    pJobBuf += FFB_SCTSIZE;

                    _PutFreeBuf(pCtx, nL2);
                    *pL2Tbl  = FFB_NILL32;

                    FFB_ASSERT(pClt->pL1Cnt[nL1Idx] > 0);            
                    pClt->pL1Cnt[nL1Idx] -= 1;

                    FFB_ASSERT(pClt->nSctCnt > 0);
                    pClt->nSctCnt--;
                    if (--nJobSctCnt == 0) break;
                }
            }
            if (pClt->pL1Cnt[nL1Idx] == 0)
            {
                pClt->pL1Tbl[nL1Idx] = FFB_NILL32;
                _PutFreeBuf(pCtx, nL1);
            }

            if (nL2Idx < FFB_L2SIZE) break;
        }
        if (nJobSctCnt == 0) break;
    }
    if (pClt->nSctCnt == 0) 
    {
        _DeQueueAndFreeClt(pCtx, pClt);
    }
}

static U32 
_GetFreeBuf(FFBCTX* pCtx)
{
    U32 nBIdx;

    nBIdx = pCtx->nFreeBufHead;
    if (nBIdx != FFB_NILL32)
    {
        pCtx->nFreeBufHead = *((U32*)FFB_BUFADDR(pCtx, nBIdx));
        pCtx->nFreeBufCnt--;
    }

    return nBIdx;
}

static void
_PutFreeBuf(FFBCTX* pCtx, U32 nBIdx)
{
    *((U32*)FFB_BUFADDR(pCtx, nBIdx)) = pCtx->nFreeBufHead;
    pCtx->nFreeBufHead            = nBIdx;
    pCtx->nFreeBufCnt++;
}

static CLTHDR* 
_InitCltHdr(FFBCTX* pCtx, U32 nLcn, U32 nTimeTick)
{
    CLTHDR* pClt;

    pClt = (CLTHDR*)FFB_BUFADDR(pCtx, pCtx->pCltTbl[nLcn]);
    memset(pClt, 0, sizeof(CLTHDR));

    pClt->nTimeStamp = nTimeTick;
    pClt->nCltNum    = nLcn;
    pClt->pL1Tbl     = (U32*)((U08*)pClt + sizeof(CLTHDR));
    pClt->pL1Cnt     = (U08*)(pClt->pL1Tbl + pCtx->nL1Size);

    memset(pClt->pL1Tbl, FFB_NILL32, pCtx->nL1Size * sizeof(U32));
    memset(pClt->pL1Cnt, 0, pCtx->nL1Size * sizeof(U08));

    /* Insert cluster header into LRU list */
    _InsertToLRUList(pCtx, pClt);

    /* Insert cluster header into Deadline queue */
    _InsertToDLQ(pCtx, pClt);

    pCtx->nNumOfCltInBuf++;

    return pClt;
}

static void
_InsertToLRUList(FFBCTX* pCtx, CLTHDR* pClt)
{
    pClt->pNext = pCtx->pCLHead;
    if (pCtx->pCLHead == NULL)
    {
        pClt->pPrev = pClt;
    }
    else
    {
        pClt->pPrev          = pCtx->pCLHead->pPrev;
        pCtx->pCLHead->pPrev = pClt;
    }
    pCtx->pCLHead = pClt;
}

static void
_RemoveFromLRUList(FFBCTX* pCtx, CLTHDR* pClt)
{
    /* Remove from LRU list */
    if (pCtx->pCLHead == pClt)
    {
        pCtx->pCLHead = pClt->pNext;
        if (pClt->pNext) pClt->pNext->pPrev = pClt->pPrev;
    }
    else
    {
        pClt->pPrev->pNext = pClt->pNext;
        if (pClt->pNext == NULL)
        {
            pCtx->pCLHead->pPrev = pClt->pPrev;
        }
        else
        {
            pClt->pNext->pPrev = pClt->pPrev;
        }
    }
}

static void
_InsertToDLQ(FFBCTX* pCtx, CLTHDR* pClt)
{
    if (pCtx->pCLOldest == NULL)
    {
        pCtx->pCLOldest = pClt;
        pClt->pOlder    = pClt;
    }
    else
    {
        FFB_ASSERT(pCtx->pCLOldest->pNewer == NULL);

        pClt->pOlder            = pCtx->pCLOldest->pOlder;
        pCtx->pCLOldest->pOlder = pClt;
        pClt->pOlder->pNewer    = pClt;
    }
}

static void
_RemoveFromDLQ(FFBCTX* pCtx, CLTHDR* pClt)
{
    /* Remove from Deadline Queue */
    if (pCtx->pCLOldest == pClt)    /* When the queue head is removed */
    {
        pCtx->pCLOldest = pClt->pNewer;
        if (pClt->pNewer) pClt->pNewer->pOlder = pClt->pOlder;
    }
    else
    {
        pClt->pOlder->pNewer = pClt->pNewer;
        if (pClt->pNewer == NULL)
        {
            pCtx->pCLOldest->pOlder = pClt->pOlder;
        }
        else
        {
            pClt->pNewer->pOlder = pClt->pOlder;
        }
    }
}


static void
_DeQueueAndFreeClt(FFBCTX* pCtx, CLTHDR* pClt)
{
    U32 nClt;

    if (pCtx->pVictimClt == pClt) 
    {
        pCtx->pVictimClt = NULL;
    }

    _RemoveFromLRUList(pCtx, pClt);
    _RemoveFromDLQ(pCtx, pClt);


    nClt = pCtx->pCltTbl[pClt->nCltNum];
    FFB_ASSERT(FFB_BUFADDR(pCtx, nClt) == (U08*)pClt);

    _PutFreeBuf(pCtx, nClt);
    pCtx->pCltTbl[pClt->nCltNum] = FFB_NILL32;

    FFB_ASSERT(pCtx->nNumOfCltInBuf > 0);
    pCtx->nNumOfCltInBuf--;
}

static void
_RollLeft(FFBCTX* pCtx)
{
    CLTHDR* pClt;

    pClt = pCtx->pCLHead;
    if ((pClt) && (pClt->pNext))
    {
        pCtx->pCLHead = pClt->pNext;
        FFB_ASSERT(pClt->pPrev->pNext == NULL);
        pClt->pPrev->pNext = pClt;
        pClt->pNext        = NULL;
    }
}