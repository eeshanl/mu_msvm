/** @file
    Bounce buffer types and declarations for the Hyper-V IOMMU driver.

    This module provides bounce buffering for DMA operations in isolated
    Hyper-V virtual machines. Memory that needs to be visible to the host
    for DMA must be explicitly shared via the hypervisor.

    Copyright (c) Microsoft Corporation.
    SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/EfiHv.h>
#include <Protocol/IoMmu.h>
#include <IsolationTypes.h>

//
// Default bounce block size: 32 pages (128KB) per block.
//
#define IOMMU_BOUNCE_BLOCK_SIZE  (32 * EFI_PAGE_SIZE)

//
// IOMMU_BOUNCE_BLOCK - a large contiguous region of host-visible memory
// that is subdivided into individual pages for bounce buffering.
//
typedef struct _IOMMU_BOUNCE_BLOCK
{
    LIST_ENTRY                    BlockListEntry;

    struct _IOMMU_BOUNCE_PAGE     *FreePageListHead;

    UINT32                        InUsePageCount;
    BOOLEAN                       IsHostVisible;

    VOID                          *BlockBase;
    UINT32                        BlockPageCount;
    EFI_HV_PROTECTION_HANDLE     ProtectionHandle;

    // Allocated as a single array for all pages in this block.
    struct _IOMMU_BOUNCE_PAGE     *BouncePageStructureBase;
} IOMMU_BOUNCE_BLOCK, *PIOMMU_BOUNCE_BLOCK;

//
// IOMMU_BOUNCE_PAGE - represents one guest physical page within a block.
// Pages are acquired for an I/O operation and returned to the block pool
// when the I/O completes.
//
typedef struct _IOMMU_BOUNCE_PAGE
{
    struct _IOMMU_BOUNCE_PAGE     *NextBouncePage;
    struct _IOMMU_BOUNCE_BLOCK    *BounceBlock;
    VOID                          *PageVA;
    UINT64                        HostVisiblePA;
} IOMMU_BOUNCE_PAGE, *PIOMMU_BOUNCE_PAGE;

//
// Context for tracking host visibility of an address range.
//
typedef struct _IOMMU_HOST_VISIBILITY_CONTEXT
{
    EFI_HV_PROTECTION_HANDLE     RangeProtectionHandle;
} IOMMU_HOST_VISIBILITY_CONTEXT;

//
// MAP_CONTEXT - tracking structure for an active Map operation.
// Stored as the Mapping handle returned to callers.
//
// The IOMMU protocol contract requires that [DeviceAddress, DeviceAddress +
// NumberOfBytes) is a contiguous DMA-visible region. To satisfy this, Map
// allocates fresh contiguous pages via AllocatePages and makes them host-
// visible, rather than using the bounce page pool (which can return non-
// contiguous pages after interleaved acquire/release).
//
#define IOMMU_MAP_CONTEXT_SIGNATURE  SIGNATURE_32('i','o','m','c')

typedef struct _IOMMU_MAP_CONTEXT
{
    UINT32                        Signature;
    EDKII_IOMMU_OPERATION         Operation;
    VOID                          *HostAddress;
    UINTN                         NumberOfBytes;
    VOID                          *BounceBase;
    UINT32                        BouncePageCount;
    IOMMU_HOST_VISIBILITY_CONTEXT VisibilityContext;
} IOMMU_MAP_CONTEXT, *PIOMMU_MAP_CONTEXT;

//
// ALLOC_CONTEXT - tracking structure for an active AllocateBuffer allocation.
// Used by FreeBuffer to revoke host visibility.
//
typedef struct _IOMMU_ALLOC_CONTEXT
{
    LIST_ENTRY                    Link;
    VOID                          *OriginalAddress;
    UINTN                         Pages;
    IOMMU_HOST_VISIBILITY_CONTEXT VisibilityContext;
} IOMMU_ALLOC_CONTEXT, *PIOMMU_ALLOC_CONTEXT;


//
// Bounce buffer initialization.
//
EFI_STATUS
IoMmuInitializeBounce (
    VOID
    );

//
// Returns TRUE if bounce buffering is active (isolated VM with IOMMU).
//
BOOLEAN
IoMmuIsBounceActive (
    VOID
    );

//
// Allocate a large block of host-visible memory for bounce buffering.
//
EFI_STATUS
IoMmuAllocateBounceBlock (
    IN LIST_ENTRY   *BounceBlockListHead,
    IN UINT32       BlockByteCount
    );

//
// Free a single bounce block.
//
VOID
IoMmuFreeBounceBlock (
    IN PIOMMU_BOUNCE_BLOCK  Block
    );

//
// Free all bounce blocks from the list.
//
VOID
IoMmuFreeAllBounceBlocks (
    IN LIST_ENTRY   *BounceBlockListHead
    );

//
// Acquire bounce pages from the pool for an I/O operation.
//
PIOMMU_BOUNCE_PAGE
IoMmuAcquireBouncePages (
    IN LIST_ENTRY   *BounceBlockListHead,
    IN UINT32       PageCount
    );

//
// Release bounce pages back to the pool after an I/O completes.
//
VOID
IoMmuReleaseBouncePages (
    IN PIOMMU_BOUNCE_PAGE   BounceListHead
    );

//
// Copy data between bounce pages and an external buffer.
//
VOID
IoMmuCopyBouncePagesToExternalBuffer (
    IN VOID               *ExternalBuffer,
    IN UINT32             BufferSize,
    IN PIOMMU_BOUNCE_PAGE BouncePageList,
    IN BOOLEAN            CopyToBounce
    );

//
// Zero all bounce pages in a list.
//
VOID
IoMmuZeroBouncePageList (
    IN PIOMMU_BOUNCE_PAGE   BouncePageList
    );

//
// Address translation helpers for shared GPA.
//
UINTN
IoMmuGetSharedPa (
    IN VOID     *Address
    );

VOID *
IoMmuGetSharedVa (
    IN VOID     *Address
    );

//
// Host visibility management.
//
EFI_STATUS
IoMmuMakeAddressRangeShared (
    IN VOID                             *BaseAddress,
    IN UINT32                           PageCount,
    OUT IOMMU_HOST_VISIBILITY_CONTEXT   *VisibilityContext
    );

VOID
IoMmuMakeAddressRangePrivate (
    IN IOMMU_HOST_VISIBILITY_CONTEXT    *VisibilityContext
    );
