/** @file
    Hyper-V IOMMU DXE driver.

    Implements the EDKII_IOMMU_PROTOCOL for Hyper-V isolated virtual machines.
    Provides bounce buffering so that DMA operations use host-visible memory.
    This driver only installs the protocol when running in an isolated VM;
    non-isolated VMs do not need IOMMU-based DMA translation.

    This is a generic implementation: any driver that uses PciIo or IoMmuLib
    for DMA (e.g., NvmExpressDxe, StorvscDxe) benefits from this bounce
    buffering transparently.

    Copyright (c) Microsoft Corporation.
    SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "IoMmuBounce.h"

#include <IsolationTypes.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/IoMmu.h>

//
// External globals from IoMmuBounce.c
//
extern LIST_ENTRY  mAllocContextListHead;


/**
  EDKII_IOMMU_PROTOCOL.SetAttribute - Set IOMMU access attributes.

  For Hyper-V isolation, this is a no-op since visibility is managed
  explicitly through the bounce buffer mechanism rather than hardware
  page tables.

  @param[in]  This          Protocol instance.
  @param[in]  DeviceHandle  Device requesting access.
  @param[in]  Mapping       Mapping handle from Map().
  @param[in]  IoMmuAccess   Access flags (READ/WRITE).

  @retval EFI_SUCCESS       Always succeeds.
**/
STATIC
EFI_STATUS
EFIAPI
IoMmuSetAttribute (
    IN EDKII_IOMMU_PROTOCOL    *This,
    IN EFI_HANDLE              DeviceHandle,
    IN VOID                    *Mapping,
    IN UINT64                  IoMmuAccess
    )
{
    return EFI_SUCCESS;
}


/**
  EDKII_IOMMU_PROTOCOL.Map - Map a host address for DMA.

  For BusMasterCommonBuffer: the memory was already made host-visible by
  AllocateBuffer; returns the shared physical address directly.

  For BusMasterRead (host→device): acquires bounce pages, copies data into
  them, and returns the bounce physical address.

  For BusMasterWrite (device→host): acquires bounce pages, zeroes them,
  and returns the bounce physical address. Data is copied back on Unmap.

  @param[in]      This            Protocol instance.
  @param[in]      Operation       DMA operation type.
  @param[in]      HostAddress     System memory address to map.
  @param[in,out]  NumberOfBytes   Bytes to map / bytes mapped.
  @param[out]     DeviceAddress   Resulting DMA address for the device.
  @param[out]     Mapping         Opaque handle for Unmap().

  @retval EFI_SUCCESS             Mapping created successfully.
  @retval EFI_OUT_OF_RESOURCES    Could not allocate bounce pages.
**/
STATIC
EFI_STATUS
EFIAPI
IoMmuMap (
    IN     EDKII_IOMMU_PROTOCOL    *This,
    IN     EDKII_IOMMU_OPERATION   Operation,
    IN     VOID                    *HostAddress,
    IN OUT UINTN                   *NumberOfBytes,
    OUT    EFI_PHYSICAL_ADDRESS    *DeviceAddress,
    OUT    VOID                    **Mapping
    )
{
    if (!IoMmuIsBounceActive ()) {
        //
        // Non-isolated: identity mapping.
        //
        *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
        *Mapping = NULL;
        return EFI_SUCCESS;
    }

    //
    // For BusMasterCommonBuffer, the memory was already made host-visible
    // via AllocateBuffer. Just return the shared PA.
    //
    if (Operation == EdkiiIoMmuOperationBusMasterCommonBuffer ||
        Operation == EdkiiIoMmuOperationBusMasterCommonBuffer64) {
        *DeviceAddress = (EFI_PHYSICAL_ADDRESS)IoMmuGetSharedPa (HostAddress);
        *Mapping = NULL;
        return EFI_SUCCESS;
    }

    //
    // For BusMasterRead/Write, allocate a contiguous bounce buffer.
    //
    // The IOMMU protocol contract requires that the returned DeviceAddress
    // range is contiguous. We cannot use the bounce page pool here because
    // it returns pages from a singly-linked free list that may be non-
    // contiguous after interleaved acquire/release operations. Instead,
    // allocate fresh contiguous pages and make them host-visible.
    //
    {
        PIOMMU_MAP_CONTEXT              MapContext;
        UINT32                          BouncePageCount;
        VOID                            *BounceBase;
        VOID                            *SharedVa;
        EFI_STATUS                      Status;
        IOMMU_HOST_VISIBILITY_CONTEXT   VisibilityContext;
        EFI_PHYSICAL_ADDRESS            BouncePA;
        EFI_PHYSICAL_ADDRESS            DmaMemoryTop;

        BouncePageCount = (UINT32)EFI_SIZE_TO_PAGES (*NumberOfBytes);

        DmaMemoryTop = MAX_UINTN;
        if ((Operation != EdkiiIoMmuOperationBusMasterRead64) &&
            (Operation != EdkiiIoMmuOperationBusMasterWrite64))
        {
            //
            // If the device cannot handle DMA above 4GB but any part of the
            // bounce buffer would be above 4GB, constrain it below 4GB.
            //
            DmaMemoryTop = SIZE_4GB - 1;
        }

        BouncePA = DmaMemoryTop;
        Status   = gBS->AllocatePages (
                            AllocateMaxAddress,
                            EfiBootServicesData,
                            BouncePageCount,
                            &BouncePA
                            );
        if (EFI_ERROR (Status)) {
            DEBUG ((DEBUG_ERROR,
                "IoMmuMap: Failed to allocate %d pages (DmaMemoryTop=0x%lx): %r\n",
                BouncePageCount, DmaMemoryTop, Status));
            return EFI_OUT_OF_RESOURCES;
        }

        BounceBase = (VOID *)(UINTN)BouncePA;

        //
        // Make the contiguous region host-visible for DMA.
        //
        Status = IoMmuMakeAddressRangeShared (
                     BounceBase,
                     BouncePageCount,
                     &VisibilityContext
                     );
        if (EFI_ERROR (Status)) {
            FreePages (BounceBase, BouncePageCount);
            return Status;
        }

        SharedVa = IoMmuGetSharedVa (BounceBase);

        //
        // Zero the entire bounce buffer. For BusMasterRead (host->device),
        // this prevents leaking guest data in unused page fragments. For
        // BusMasterWrite (device->host), this provides a clean buffer that
        // the device will write into (data copied back on Unmap).
        //
        ZeroMem (SharedVa, (UINTN)BouncePageCount * EFI_PAGE_SIZE);

        if (Operation == EdkiiIoMmuOperationBusMasterRead ||
            Operation == EdkiiIoMmuOperationBusMasterRead64) {
            //
            // Host->device transfer: copy data into the zeroed bounce buffer.
            //
            CopyMem (SharedVa, HostAddress, *NumberOfBytes);
        }

        //
        // Return the host-visible physical address.
        //
        *DeviceAddress = (EFI_PHYSICAL_ADDRESS)IoMmuGetSharedPa (BounceBase);

        //
        // Allocate mapping context for Unmap.
        //
        MapContext = AllocateZeroPool (sizeof (IOMMU_MAP_CONTEXT));
        if (MapContext == NULL) {
            IoMmuMakeAddressRangePrivate (&VisibilityContext);
            FreePages (BounceBase, BouncePageCount);
            return EFI_OUT_OF_RESOURCES;
        }

        MapContext->Signature         = IOMMU_MAP_CONTEXT_SIGNATURE;
        MapContext->Operation         = Operation;
        MapContext->HostAddress       = HostAddress;
        MapContext->NumberOfBytes     = *NumberOfBytes;
        MapContext->BounceBase        = BounceBase;
        MapContext->BouncePageCount   = BouncePageCount;
        MapContext->VisibilityContext = VisibilityContext;

        *Mapping = MapContext;
    }

    return EFI_SUCCESS;
}


/**
  EDKII_IOMMU_PROTOCOL.Unmap - Complete a Map operation and release resources.

  For BusMasterWrite operations, copies data from the bounce buffer back
  to the original host address before releasing the bounce pages.

  @param[in]  This      Protocol instance.
  @param[in]  Mapping   Mapping handle from Map().

  @retval EFI_SUCCESS   Unmap completed.
**/
STATIC
EFI_STATUS
EFIAPI
IoMmuUnmap (
    IN EDKII_IOMMU_PROTOCOL    *This,
    IN VOID                    *Mapping
    )
{
    PIOMMU_MAP_CONTEXT  MapContext;

    if (Mapping == NULL) {
        //
        // NULL mapping: BusMasterCommonBuffer or no-op. Nothing to do.
        //
        return EFI_SUCCESS;
    }

    MapContext = (PIOMMU_MAP_CONTEXT)Mapping;
    ASSERT (MapContext->Signature == IOMMU_MAP_CONTEXT_SIGNATURE);

    //
    // For BusMasterWrite (device->host), copy data from bounce to host.
    //
    if (MapContext->Operation == EdkiiIoMmuOperationBusMasterWrite ||
        MapContext->Operation == EdkiiIoMmuOperationBusMasterWrite64) {
        VOID  *SharedVa;

        SharedVa = IoMmuGetSharedVa (MapContext->BounceBase);
        CopyMem (
            MapContext->HostAddress,
            SharedVa,
            MapContext->NumberOfBytes
            );
    }

    //
    // Revoke host visibility and free the contiguous bounce pages.
    // Unlike FreeBuffer (which leaks pages as an ND2 workaround for
    // common buffers), Map/Unmap bounce pages are transient and can
    // be freed after revoking host visibility.
    //
    if ((MapContext->Operation != EdkiiIoMmuOperationBusMasterCommonBuffer) &&
      (MapContext->Operation != EdkiiIoMmuOperationBusMasterCommonBuffer64))
    {
        IoMmuMakeAddressRangePrivate (&MapContext->VisibilityContext);
        FreePages (MapContext->BounceBase, MapContext->BouncePageCount);
        FreePool (MapContext);
    }

    return EFI_SUCCESS;
}


/**
  EDKII_IOMMU_PROTOCOL.AllocateBuffer - Allocate DMA-capable memory.

  Allocates pages and makes them host-visible so the device can DMA to them.
  Returns a canonicalized shared VA that maps above the shared GPA boundary.

  When EDKII_IOMMU_ATTRIBUTE_DUAL_ADDRESS_CYCLE is not set the allocation is
  constrained below 4GB and Type is forced to AllocateMaxAddress.  When the
  attribute is set, the caller-supplied Type is used as-is.

  @param[in]      This          Protocol instance.
  @param[in]      Type          Allocation type; overridden to AllocateMaxAddress
                                when dual-address-cycle is not set.
  @param[in]      MemoryType    Memory type (EfiBootServicesData or EfiRuntimeServicesData).
  @param[in]      Pages         Number of pages to allocate.
  @param[in,out]  HostAddress   On output, the shared VA pointer.
  @param[in]      Attributes    Allocation attributes (EDKII_IOMMU_ATTRIBUTE_*).

  @retval EFI_SUCCESS              Memory allocated and made host-visible.
  @retval EFI_INVALID_PARAMETER    This, HostAddress, or Pages is invalid.
  @retval EFI_OUT_OF_RESOURCES     Allocation or visibility call failed.
**/
STATIC
EFI_STATUS
EFIAPI
IoMmuAllocateBuffer (
    IN     EDKII_IOMMU_PROTOCOL    *This,
    IN     EFI_ALLOCATE_TYPE       Type,
    IN     EFI_MEMORY_TYPE         MemoryType,
    IN     UINTN                   Pages,
    IN OUT VOID                    **HostAddress,
    IN     UINT64                  Attributes
    )
{
    EFI_STATUS                    Status;
    IOMMU_HOST_VISIBILITY_CONTEXT VisibilityContext;
    PIOMMU_ALLOC_CONTEXT          AllocContext;
    EFI_PHYSICAL_ADDRESS          PhysicalAddress;

    if ((This == NULL) || (Pages == 0) || (HostAddress == NULL)) {
        DEBUG ((DEBUG_ERROR, "IoMmuAllocateBuffer: Invalid parameter\n"));
        Status = EFI_INVALID_PARAMETER;
        goto End;
    }

    if ((Attributes & EDKII_IOMMU_ATTRIBUTE_DUAL_ADDRESS_CYCLE) == 0) {
        //
        // Device cannot address above 4GB; constrain allocation below 4GB.
        //
        PhysicalAddress = SIZE_4GB - 1;
        Type            = AllocateMaxAddress;
    }

    Status = gBS->AllocatePages (
                     Type,
                     MemoryType,
                     Pages,
                     &PhysicalAddress
                     );
    if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "IoMmuAllocateBuffer: Failed to allocate pages: %r\n", Status));
        goto End;
    }

    if (!IoMmuIsBounceActive ()) {
        //
        // Non-isolated: just return the allocated buffer directly.
        //
        *HostAddress = (VOID *)(UINTN)PhysicalAddress;
        goto End;
    }

    //
    // Make the allocated range host-visible for DMA.
    //
    Status = IoMmuMakeAddressRangeShared (
                 (VOID *)(UINTN)PhysicalAddress,
                 (UINT32)Pages,
                 &VisibilityContext
                 );
    if (EFI_ERROR (Status)) {
        gBS->FreePages (PhysicalAddress, Pages);
        goto End;
    }

    //
    // Track this allocation so FreeBuffer can revoke visibility.
    //
    AllocContext = AllocateZeroPool (sizeof (IOMMU_ALLOC_CONTEXT));
    if (AllocContext == NULL) {
        IoMmuMakeAddressRangePrivate (&VisibilityContext);
        gBS->FreePages (PhysicalAddress, Pages);
        Status = EFI_OUT_OF_RESOURCES;
        goto End;
    }

    AllocContext->OriginalAddress   = (VOID *)(UINTN)PhysicalAddress;
    AllocContext->Pages             = Pages;
    AllocContext->VisibilityContext = VisibilityContext;
    InsertTailList (&mAllocContextListHead, &AllocContext->Link);

    //
    // Return the canonicalized shared VA.
    //
    *HostAddress = IoMmuGetSharedVa ((VOID *)(UINTN)PhysicalAddress);

End:
    ASSERT_EFI_ERROR (Status);
    return Status;
}


/**
  EDKII_IOMMU_PROTOCOL.FreeBuffer - Free DMA-capable memory.

  Revokes host visibility for the allocation. Pages are intentionally
  NOT freed to work around a host ND2 bug where the host registers
  write notifications on freed pages, causing the VM to hang.
  The pages will be reclaimed at ExitBootServices.

  @param[in]  This          Protocol instance.
  @param[in]  Pages         Number of pages to free.
  @param[in]  HostAddress   The shared VA returned by AllocateBuffer.

  @retval EFI_SUCCESS       Visibility revoked (pages leaked intentionally).
**/
STATIC
EFI_STATUS
EFIAPI
IoMmuFreeBuffer (
    IN EDKII_IOMMU_PROTOCOL    *This,
    IN UINTN                   Pages,
    IN VOID                    *HostAddress
    )
{
    LIST_ENTRY          *Entry;
    PIOMMU_ALLOC_CONTEXT AllocContext;

    if (!IoMmuIsBounceActive ()) {
        //
        // Non-isolated: free the pages directly. There is no host-visibility
        // / ND2 concern outside of isolated VMs, so we release them normally.
        //
        return gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress, Pages);
    }

    //
    // Find the tracking entry and revoke host visibility.
    //
    for (Entry = GetFirstNode (&mAllocContextListHead);
         !IsNull (&mAllocContextListHead, Entry);
         Entry = GetNextNode (&mAllocContextListHead, Entry)) {

        AllocContext = BASE_CR (Entry, IOMMU_ALLOC_CONTEXT, Link);

        if (IoMmuGetSharedVa (AllocContext->OriginalAddress) == HostAddress) {
            RemoveEntryList (Entry);
            IoMmuMakeAddressRangePrivate (&AllocContext->VisibilityContext);
            FreePool (AllocContext);
            break;
        }
    }

    //
    // NOTE: Pages are intentionally NOT freed to work around a bug with
    // ND2 on the host registering write notifications for pages, resulting
    // in the VM hanging. These pages will be reclaimed at ExitBootServices.
    //
    // FreePages (HostAddress, Pages);

    return EFI_SUCCESS;
}


//
// Protocol instance.
//
STATIC EDKII_IOMMU_PROTOCOL  mIoMmuProtocol = {
    EDKII_IOMMU_PROTOCOL_REVISION,
    IoMmuSetAttribute,
    IoMmuMap,
    IoMmuUnmap,
    IoMmuAllocateBuffer,
    IoMmuFreeBuffer
};


/**
  IoMmuDxe driver entry point.

  Initializes the bounce buffer subsystem and installs the EDKII_IOMMU_PROTOCOL.
  In isolated VMs, the protocol provides bounce buffering for DMA. In non-isolated
  VMs, the protocol provides simple pass-through behavior (identity mapping,
  standard allocation). The protocol is always installed to satisfy the
  IoMmuLib AARCH64 DEPEX requirement.

  @param[in]  ImageHandle   Handle for this driver image.
  @param[in]  SystemTable   Pointer to the UEFI system table.

  @retval EFI_SUCCESS       Protocol installed successfully.
  @retval other             Initialization failed.
**/
EFI_STATUS
EFIAPI
IoMmuDxeEntryPoint (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE  *SystemTable
    )
{
    EFI_STATUS  Status;

    if (IsIsolated ()) {
        //
        // Isolated VM: initialize bounce buffer subsystem (caches PCDs,
        // locates the HV IVM protocol, initializes list heads).
        //
        Status = IoMmuInitializeBounce ();
        if (EFI_ERROR (Status)) {
            DEBUG ((DEBUG_ERROR, "IoMmuDxe: Failed to initialize bounce buffers: %r\n", Status));
            return Status;
        }

        DEBUG ((DEBUG_INFO, "IoMmuDxe: Bounce buffers initialized for isolated VM\n"));
    } else {
        //
        // Non-isolated VM: initialize allocation tracking list head.
        // No bounce blocks or HV protocol needed.
        //
        InitializeListHead (&mAllocContextListHead);
        DEBUG ((DEBUG_INFO, "IoMmuDxe: Non-isolated VM, installing pass-through IOMMU protocol\n"));
    }

    //
    // Always install the IOMMU protocol. On AARCH64, the IoMmuLib has a DEPEX
    // on this protocol, so it must be available for VpcivscDxe (and any other
    // IoMmuLib consumer) to load.
    //
    Status = gBS->InstallProtocolInterface (
                     &ImageHandle,
                     &gEdkiiIoMmuProtocolGuid,
                     EFI_NATIVE_INTERFACE,
                     &mIoMmuProtocol
                     );
    if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "IoMmuDxe: Failed to install IOMMU protocol: %r\n", Status));
        return Status;
    }

    DEBUG ((DEBUG_INFO, "IoMmuDxe: IOMMU protocol installed\n"));

    return EFI_SUCCESS;
}
