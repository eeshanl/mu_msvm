/** @file
    Bounce buffer implementation for the Hyper-V IOMMU driver.

    Provides host-visible bounce buffer management for DMA operations in
    isolated Hyper-V virtual machines. Adapted from the NvmExpressBounce
    implementation to be generic and usable by any DMA-capable driver.

    Copyright (c) Microsoft Corporation.
    SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "IoMmuBounce.h"

#include <IsolationTypes.h>
#include <Library/PcdLib.h>

//
// Module globals for host visibility and shared GPA translation.
//
EFI_HV_IVM_PROTOCOL  *mHvIvm;
UINTN                mSharedGpaBoundary;
UINT64               mCanonicalizationMask;

//
// Global bounce block pool and allocation tracking.
//
LIST_ENTRY           mBounceBlockListHead;
LIST_ENTRY           mAllocContextListHead;


/**
  Initialize the bounce buffer subsystem. Caches PCDs and locates
  the hypervisor IVM protocol.

  @retval EFI_SUCCESS           Initialization successful.
  @retval other                 Failed to locate the HV IVM protocol.
**/
EFI_STATUS
IoMmuInitializeBounce (
    VOID
    )
{
    InitializeListHead (&mBounceBlockListHead);
    InitializeListHead (&mAllocContextListHead);

    mSharedGpaBoundary = (UINTN)PcdGet64 (PcdIsolationSharedGpaBoundary);
    mCanonicalizationMask = PcdGet64 (PcdIsolationSharedGpaCanonicalizationBitmask);

    return gBS->LocateProtocol (&gEfiHvIvmProtocolGuid, NULL, (VOID **)&mHvIvm);
}


/**
  Return TRUE if bounce buffering should be used for DMA operations.

  @retval TRUE    The VM is isolated and bounce buffering is required.
  @retval FALSE   No isolation; DMA can access all memory directly.
**/
BOOLEAN
IoMmuIsBounceActive (
    VOID
    )
{
    return IsIsolated ();
}


/**
  Allocate a large block of host-visible memory for bounce buffering.
  The block is subdivided into individual pages tracked by IOMMU_BOUNCE_PAGE
  structures.

  @param[in]  BounceBlockListHead   List to insert the new block into.
  @param[in]  BlockByteCount        Number of bytes to allocate. Must be page-aligned.

  @retval EFI_SUCCESS               Block allocated and added to the list.
  @retval EFI_INVALID_PARAMETER     BlockByteCount is not page-aligned.
  @retval EFI_OUT_OF_RESOURCES      Memory allocation or visibility call failed.
**/
EFI_STATUS
IoMmuAllocateBounceBlock (
    IN LIST_ENTRY   *BounceBlockListHead,
    IN UINT32       BlockByteCount
    )
{
    EFI_STATUS          Status = EFI_INVALID_PARAMETER;
    UINT32              PageCount = 0;
    UINT32              Index;
    PIOMMU_BOUNCE_BLOCK BounceBlock = NULL;
    UINT8               *NextVa;
    UINT64              NextPa;

    DEBUG ((DEBUG_VERBOSE,
        "%a(%d) ByteCount=0x%x\n",
        __FUNCTION__, __LINE__, BlockByteCount));

    if (BlockByteCount % EFI_PAGE_SIZE) {
        Status = EFI_INVALID_PARAMETER;
        goto Cleanup;
    }

    PageCount = BlockByteCount / EFI_PAGE_SIZE;

    BounceBlock = AllocatePool (sizeof (*BounceBlock));
    if (BounceBlock == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        goto Cleanup;
    }

    ZeroMem (BounceBlock, sizeof (*BounceBlock));

    //
    // Allocate the bounce page memory.
    //
    BounceBlock->BlockBase = AllocatePages (PageCount);
    if (BounceBlock->BlockBase == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        goto Cleanup;
    }

    BounceBlock->BlockPageCount = PageCount;
    ZeroMem (BounceBlock->BlockBase, PageCount * EFI_PAGE_SIZE);

    //
    // Allocate tracking structures as a single array.
    //
    BounceBlock->BouncePageStructureBase = AllocatePool (PageCount * sizeof (IOMMU_BOUNCE_PAGE));
    if (BounceBlock->BouncePageStructureBase == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        goto Cleanup;
    }

    BounceBlock->FreePageListHead = BounceBlock->BouncePageStructureBase;
    NextVa = BounceBlock->BlockBase;
    NextPa = (UINT64)NextVa;

    //
    // Make the pages visible to the host for DMA.
    //
    if (IsIsolated ()) {
        Status = mHvIvm->MakeAddressRangeHostVisible (
                     mHvIvm,
                     HV_MAP_GPA_READABLE | HV_MAP_GPA_WRITABLE,
                     BounceBlock->BlockBase,
                     PageCount * EFI_PAGE_SIZE,
                     FALSE,
                     &BounceBlock->ProtectionHandle
                     );

        if (EFI_ERROR (Status)) {
            goto Cleanup;
        }

        //
        // Adjust the address above the shared GPA boundary.
        //
        NextPa += mSharedGpaBoundary;

        //
        // Canonicalize the VA.
        //
        NextVa = (VOID *)(mCanonicalizationMask | NextPa);
        BounceBlock->IsHostVisible = TRUE;
    }

    //
    // Initialize the per-page tracking structures as a free list.
    //
    for (Index = 0; Index < PageCount; Index++) {
        if (Index == (PageCount - 1)) {
            BounceBlock->BouncePageStructureBase[Index].NextBouncePage = NULL;
        } else {
            BounceBlock->BouncePageStructureBase[Index].NextBouncePage =
                &BounceBlock->BouncePageStructureBase[Index + 1];
        }

        BounceBlock->BouncePageStructureBase[Index].BounceBlock = BounceBlock;
        BounceBlock->BouncePageStructureBase[Index].PageVA = NextVa;
        BounceBlock->BouncePageStructureBase[Index].HostVisiblePA = NextPa;
        NextVa += EFI_PAGE_SIZE;
        NextPa += EFI_PAGE_SIZE;
    }

    InsertTailList (BounceBlockListHead, &BounceBlock->BlockListEntry);
    Status = EFI_SUCCESS;

Cleanup:
    DEBUG ((DEBUG_INFO,
        "%a(%d) BounceBlock=%p Status=0x%x\n",
        __FUNCTION__, __LINE__, BounceBlock, Status));

    if (EFI_ERROR (Status)) {
        if (BounceBlock != NULL) {
            IoMmuFreeBounceBlock (BounceBlock);
            BounceBlock = NULL;
        }
    }

    return Status;
}


/**
  Free a single bounce block. Revokes host visibility and frees all memory.

  @param[in]  Block     Bounce block to free.
**/
VOID
IoMmuFreeBounceBlock (
    IN PIOMMU_BOUNCE_BLOCK  Block
    )
{
    if (Block->IsHostVisible) {
        mHvIvm->MakeAddressRangeNotHostVisible (mHvIvm, Block->ProtectionHandle);
    }

    if (Block->BouncePageStructureBase != NULL) {
        FreePool (Block->BouncePageStructureBase);
        Block->BouncePageStructureBase = NULL;
    }

    if (Block->BlockBase != NULL) {
        FreePages (Block->BlockBase, Block->BlockPageCount);
        Block->BlockBase = NULL;
        Block->BlockPageCount = 0;
    }

    FreePool (Block);
}


/**
  Free all bounce blocks in the list. Revokes host visibility and frees memory.

  @param[in]  BounceBlockListHead   List of bounce blocks to free.
**/
VOID
IoMmuFreeAllBounceBlocks (
    IN LIST_ENTRY   *BounceBlockListHead
    )
{
    PIOMMU_BOUNCE_BLOCK Block;
    LIST_ENTRY          *Entry;

    while (!IsListEmpty (BounceBlockListHead)) {
        Entry = GetFirstNode (BounceBlockListHead);
        RemoveEntryList (Entry);

        Block = BASE_CR (Entry, IOMMU_BOUNCE_BLOCK, BlockListEntry);

        DEBUG ((DEBUG_WARN,
            "%a(%d) Block=%p IsHostVis=%d InUsePageCount=%d BlockBase=%p PageCount=0x%x\n",
            __FUNCTION__, __LINE__,
            Block,
            Block->IsHostVisible,
            Block->InUsePageCount,
            Block->BlockBase,
            Block->BlockPageCount));

        IoMmuFreeBounceBlock (Block);
    }
}


/**
  Acquire bounce pages from the global pool for a DMA operation.
  If insufficient pages are available, allocates a new bounce block.

  @param[in]  BounceBlockListHead   Global bounce block list.
  @param[in]  PageCount             Number of pages needed.

  @retval Non-NULL    Linked list of bounce pages.
  @retval NULL        Failed to acquire the requested pages.
**/
PIOMMU_BOUNCE_PAGE
IoMmuAcquireBouncePages (
    IN LIST_ENTRY   *BounceBlockListHead,
    IN UINT32       PageCount
    )
{
    PIOMMU_BOUNCE_PAGE  ListHead = NULL;
    UINT32              PagesToGo = PageCount;
    EFI_STATUS          Status;

    DEBUG ((DEBUG_VERBOSE,
        "%a(%d) PageCount=%d\n",
        __FUNCTION__, __LINE__, PageCount));

Retry:
    if (!IsListEmpty (BounceBlockListHead)) {
        LIST_ENTRY  *BlockListEntry;

        for (BlockListEntry = BounceBlockListHead->ForwardLink;
             BlockListEntry != BounceBlockListHead;
             BlockListEntry = BlockListEntry->ForwardLink) {

            PIOMMU_BOUNCE_BLOCK BounceBlock;
            BounceBlock = BASE_CR (BlockListEntry, IOMMU_BOUNCE_BLOCK, BlockListEntry);

            while (BounceBlock->FreePageListHead != NULL && PagesToGo > 0) {
                PIOMMU_BOUNCE_PAGE BouncePage;

                BouncePage = BounceBlock->FreePageListHead;
                BounceBlock->FreePageListHead = BouncePage->NextBouncePage;

                BouncePage->NextBouncePage = ListHead;
                ListHead = BouncePage;

                BounceBlock->InUsePageCount++;
                PagesToGo--;
            }

            if (PagesToGo == 0) {
                break;
            }
        }

        if (PagesToGo > 0) {
            UINT32 AllocSize = MAX (PagesToGo * EFI_PAGE_SIZE, IOMMU_BOUNCE_BLOCK_SIZE);

            Status = IoMmuAllocateBounceBlock (BounceBlockListHead, AllocSize);
            if (EFI_ERROR (Status)) {
                DEBUG ((DEBUG_WARN,
                    "%a(%d) Bounce block allocation failure\n",
                    __FUNCTION__, __LINE__));
                goto Exit;
            }

            goto Retry;
        }
    }

Exit:
    if (PagesToGo > 0) {
        //
        // Failed to acquire all pages. Release any partial allocation.
        //
        IoMmuReleaseBouncePages (ListHead);
        ListHead = NULL;

        DEBUG ((DEBUG_WARN,
            "%a(%d) PageCount=%d Returning=NULL\n",
            __FUNCTION__, __LINE__, PageCount));
    } else {
        DEBUG ((DEBUG_VERBOSE,
            "%a(%d) PageCount=%d Returning=%p\n",
            __FUNCTION__, __LINE__, PageCount, ListHead));
    }

    return ListHead;
}


/**
  Release bounce pages back to their home blocks after an I/O completes.

  @param[in]  BounceListHead    Linked list of bounce pages to release.
**/
VOID
IoMmuReleaseBouncePages (
    IN PIOMMU_BOUNCE_PAGE   BounceListHead
    )
{
    PIOMMU_BOUNCE_PAGE  Page;
    UINT32              Count = 0;

    while (BounceListHead != NULL) {
        Page = BounceListHead;
        BounceListHead = BounceListHead->NextBouncePage;

        Page->BounceBlock->InUsePageCount--;
        Count++;

        Page->NextBouncePage = Page->BounceBlock->FreePageListHead;
        Page->BounceBlock->FreePageListHead = Page;
    }

    DEBUG ((DEBUG_VERBOSE,
        "%a(%d) Released PageCount=%d\n",
        __FUNCTION__, __LINE__, Count));
}


/**
  Copy data between bounce pages and an external buffer, respecting page
  offsets of the external buffer. Zeroes partial pages at the beginning
  and end when copying to bounce to avoid leaking guest data to the host.

  @param[in]  ExternalBuffer    The caller's data buffer.
  @param[in]  BufferSize        Size of the caller's data buffer in bytes.
  @param[in]  BouncePageList    List of bounce pages (shared with host).
  @param[in]  CopyToBounce      TRUE = copy external→bounce; FALSE = bounce→external.
**/
VOID
IoMmuCopyBouncePagesToExternalBuffer (
    IN VOID               *ExternalBuffer,
    IN UINT32             BufferSize,
    IN PIOMMU_BOUNCE_PAGE BouncePageList,
    IN BOOLEAN            CopyToBounce
    )
{
    UINT64              PageOffset;
    PIOMMU_BOUNCE_PAGE  BouncePage;
    UINT8               *BounceBuffer;
    UINT8               *BounceBufferEnd;
    UINT8               *ExtBuffer;
    UINT32              TransferToGo;
    UINT32              CopySize;

    DEBUG ((DEBUG_INFO,
        "%a(%d) ExternalBuffer=%p Size=0x%x BouncePageList=%p CopyToBounce=%d\n",
        __FUNCTION__, __LINE__,
        ExternalBuffer, BufferSize, BouncePageList, CopyToBounce));

    ASSERT (BouncePageList != NULL);

    BouncePage = BouncePageList;
    PageOffset = (UINT64)ExternalBuffer % EFI_PAGE_SIZE;

    ExtBuffer = ExternalBuffer;
    TransferToGo = BufferSize;

    while (TransferToGo > 0) {
        ASSERT (BouncePage != NULL);

        BounceBuffer = (UINT8 *)BouncePage->PageVA;

        //
        // Zero unused space before the data in the first page.
        //
        if (CopyToBounce && PageOffset > 0) {
            DEBUG ((DEBUG_VERBOSE,
                "%a(%d) Zero %p size=0x%x\n",
                __FUNCTION__, __LINE__, BouncePage->PageVA, PageOffset));
            ZeroMem (BouncePage->PageVA, PageOffset);
        }

        BounceBuffer += PageOffset;
        CopySize = EFI_PAGE_SIZE - (UINT32)PageOffset;
        PageOffset = 0;

        CopySize = MIN (CopySize, TransferToGo);
        BounceBufferEnd = BounceBuffer + CopySize;

        if (CopyToBounce) {
            DEBUG ((DEBUG_VERBOSE,
                "%a(%d) CopyToBounce dst=%p src=%p size=0x%x\n",
                __FUNCTION__, __LINE__, BounceBuffer, ExtBuffer, CopySize));
            CopyMem (BounceBuffer, ExtBuffer, CopySize);
        } else {
            DEBUG ((DEBUG_VERBOSE,
                "%a(%d) CopyToExtBuffer dst=%p src=%p size=0x%x\n",
                __FUNCTION__, __LINE__, ExtBuffer, BounceBuffer, CopySize));
            CopyMem (ExtBuffer, BounceBuffer, CopySize);
        }

        TransferToGo -= CopySize;
        ExtBuffer += CopySize;

        //
        // Zero unused space after the data in the last page.
        //
        if (TransferToGo == 0 &&
            CopyToBounce &&
            ((UINT64)BounceBuffer % EFI_PAGE_SIZE)) {
            UINT32  EndOffset = (UINT64)BounceBufferEnd % EFI_PAGE_SIZE;
            UINT32  ZeroSize = EFI_PAGE_SIZE - EndOffset;

            DEBUG ((DEBUG_VERBOSE,
                "%a(%d) Zero %p size=0x%x (from offset=0x%x)\n",
                __FUNCTION__, __LINE__, BounceBufferEnd, ZeroSize, EndOffset));
            ZeroMem (BounceBufferEnd, ZeroSize);
        }

        BouncePage = BouncePage->NextBouncePage;
    }

    ASSERT (BouncePage == NULL);
}


/**
  Zero all bounce pages in a linked list.

  @param[in]  BouncePageList    List of bounce pages to zero.
**/
VOID
IoMmuZeroBouncePageList (
    IN PIOMMU_BOUNCE_PAGE   BouncePageList
    )
{
    PIOMMU_BOUNCE_PAGE  BouncePage = BouncePageList;
    UINT32              PageCount = 0;

    while (BouncePage != NULL) {
        ZeroMem (BouncePage->PageVA, EFI_PAGE_SIZE);
        BouncePage = BouncePage->NextBouncePage;
        PageCount++;
    }

    DEBUG ((DEBUG_VERBOSE,
        "%a(%d) BouncePageList=%p zeroed %d pages\n",
        __FUNCTION__, __LINE__, BouncePageList, PageCount));
}


/**
  Given an address (VA or PA), strip canonicalization and return the
  shared GPA above the shared GPA boundary.

  @param[in]  Address   Input address.

  @retval     The shared physical address.
**/
UINTN
IoMmuGetSharedPa (
    IN VOID     *Address
    )
{
    UINTN Addr;

    Addr = (UINTN)Address;
    Addr &= ~mCanonicalizationMask;
    if (Addr < mSharedGpaBoundary) {
        Addr += mSharedGpaBoundary;
    }

    return Addr;
}


/**
  Given an address (VA or PA), return a canonicalized pointer to the
  shared GPA alias.

  @param[in]  Address   Input address.

  @retval     Canonicalized shared VA pointer.
**/
VOID *
IoMmuGetSharedVa (
    IN VOID     *Address
    )
{
    return (VOID *)(IoMmuGetSharedPa (Address) | mCanonicalizationMask);
}


/**
  Make an address range host-visible for DMA.

  @param[in]  BaseAddress         Base address of the range.
  @param[in]  PageCount           Number of pages in the range.
  @param[out] VisibilityContext   Context for revoking visibility later.

  @retval EFI_SUCCESS             Range is now host-visible.
  @retval other                   Hypervisor call failed.
**/
EFI_STATUS
IoMmuMakeAddressRangeShared (
    IN  VOID                            *BaseAddress,
    IN  UINT32                          PageCount,
    OUT IOMMU_HOST_VISIBILITY_CONTEXT   *VisibilityContext
    )
{
    EFI_STATUS  Status;

    ASSERT (IsIsolated ());

    Status = mHvIvm->MakeAddressRangeHostVisible (
                 mHvIvm,
                 HV_MAP_GPA_READABLE | HV_MAP_GPA_WRITABLE,
                 BaseAddress,
                 PageCount * EFI_PAGE_SIZE,
                 FALSE,
                 &VisibilityContext->RangeProtectionHandle
                 );

    return Status;
}


/**
  Revoke host visibility for an address range.

  @param[in]  VisibilityContext   Context from a prior MakeAddressRangeShared call.
**/
VOID
IoMmuMakeAddressRangePrivate (
    IN IOMMU_HOST_VISIBILITY_CONTEXT    *VisibilityContext
    )
{
    ASSERT (IsIsolated ());

    mHvIvm->MakeAddressRangeNotHostVisible (mHvIvm, VisibilityContext->RangeProtectionHandle);
}
