/**********************************************************************
 * Copyright (c) 2008  Red Hat, Inc.
 *
 * File: ParaNdis6-Impl.c
 *
 * This file contains NDIS6-specific implementation of driver's procedures.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
**********************************************************************/
#if NDIS60_MINIPORT

#include "ParaNdis6.h"

#ifdef WPP_EVENT_TRACING
#include "ParaNdis6-Impl.tmh"
#endif

static VOID ProcessSGListHandler(IN PDEVICE_OBJECT  pDO, IN PVOID  Reserved, IN PSCATTER_GATHER_LIST  pSGL, IN PVOID  Context);

typedef struct _tagNBLDigest
{
	ULONG nLists;
	ULONG nBuffers;
	ULONG nBytes;
}tNBLDigest;

typedef struct _tagNetBufferEntry
{
	LIST_ENTRY				list;
	PNET_BUFFER_LIST		nbl;
	PNET_BUFFER				netBuffer;
	PSCATTER_GATHER_LIST	pSGList;
	PARANDIS_ADAPTER		*pContext;
}tNetBufferEntry;

#define NBLEFLAGS_FAILED	0x0001
typedef struct _tagNetBufferListEntry
{
	PNET_BUFFER_LIST		nbl;
	LIST_ENTRY				bufferEntries;
	LONG					nBuffersMapped;
	USHORT					nBuffers;
	USHORT					nBuffersDone;
	USHORT					nBuffersWaiting;
	USHORT					mss;
	USHORT					tcpHeaderOffset;
	USHORT					flags;
	union
	{
		ULONG PriorityDataLong;
		UCHAR PriorityData[ETH_PRIORITY_HEADER_SIZE];
	};
}tNetBufferListEntry;

static FORCEINLINE BOOLEAN HAS_WAITING_PACKETS(PNET_BUFFER_LIST pNBL)
{
	tNetBufferListEntry *pble = (tNetBufferListEntry *)pNBL->Scratch;
	return pble->nBuffersWaiting != 0;
}

static FORCEINLINE USHORT NUMBER_OF_PACKETS_IN_NBL(PNET_BUFFER_LIST pNBL)
{
	tNetBufferListEntry *pble = (tNetBufferListEntry *)pNBL->Scratch;
	return pble->nBuffers;
}

/**********************************************************
Implements general-purpose memory allocation routine
Parameters:
	ULONG ulRequiredSize: block size
Return value:
	PVOID allocated memory block
	NULL on error
***********************************************************/
PVOID ParaNdis_AllocateMemory(PARANDIS_ADAPTER *pContext, ULONG ulRequiredSize)
{
	return NdisAllocateMemoryWithTagPriority(
			pContext->MiniportHandle,
			ulRequiredSize,
			PARANDIS_MEMORY_TAG,
			NormalPoolPriority);
}

/**********************************************************
Implements opening of adapter-specific configuration
Parameters:

Return value:
	NDIS_HANDLE	Handle of open configuration
	NULL on error
***********************************************************/
NDIS_HANDLE ParaNdis_OpenNICConfiguration(PARANDIS_ADAPTER *pContext)
{
	NDIS_CONFIGURATION_OBJECT co;
	NDIS_HANDLE cfg;
	NDIS_STATUS status;
	DEBUG_ENTRY(2);
	co.Header.Type = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
	co.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
	co.Header.Size = sizeof(co);
	co.Flags = 0;
	co.NdisHandle = pContext->MiniportHandle;
	status = NdisOpenConfigurationEx(&co, &cfg);
	if (status != NDIS_STATUS_SUCCESS)
		cfg = NULL;
	DEBUG_EXIT_STATUS(status == NDIS_STATUS_SUCCESS ? 2 : 0, status);
	return cfg;
}

/**********************************************************
NDIS6 implementation of setting timer
Parameters:
NDIS_HANDLE timer	- previously created timer
LONG millies		- timeout in miilies
Return value:
	TRUE if the times was already set, then it is cancelled and set again
***********************************************************/
BOOLEAN ParaNdis_SetTimer(NDIS_HANDLE timer, LONG millies)
{
	LARGE_INTEGER  DueTime;
	BOOLEAN b;
	DueTime.QuadPart = (-10000) * millies;
	b = NdisSetTimerObject(timer, DueTime, 0, NULL);
	return b;
}


/**********************************************************
NDIS6 implementation of shared memory allocation
Parameters:
	context
	tCompletePhysicalAddress *pAddresses
			the structure accumulates all our knowledge
			about the allocation (size, addresses, cacheability etc)
Return value:
	TRUE if the allocation was successful
***********************************************************/
BOOLEAN ParaNdis_InitialAllocatePhysicalMemory(
	PARANDIS_ADAPTER *pContext,
	tCompletePhysicalAddress *pAddresses)
{
	NdisMAllocateSharedMemory(
		pContext->MiniportHandle,
		pAddresses->size,
		(BOOLEAN)pAddresses->IsCached,
		&pAddresses->Virtual,
		&pAddresses->Physical);
	return pAddresses->Virtual != NULL;
}


/**********************************************************
NDIS6 implementation of shared memory freeing
Parameters:
	context
	tCompletePhysicalAddress *pAddresses
			the structure accumulates all our knowledge
			about the allocation (size, addresses, cacheability etc)
			filled by ParaNdis_InitialAllocatePhysicalMemory or
			by ParaNdis_RuntimeRequestToAllocatePhysicalMemory
***********************************************************/

VOID ParaNdis_FreePhysicalMemory(
	PARANDIS_ADAPTER *pContext,
	tCompletePhysicalAddress *pAddresses)
{

	NdisMFreeSharedMemory(
		pContext->MiniportHandle,
		pAddresses->size,
		(BOOLEAN)pAddresses->IsCached,
		pAddresses->Virtual,
		pAddresses->Physical);
}

BOOLEAN ParaNdis_SynchronizeWithInterrupt(
	PARANDIS_ADAPTER *pContext,
	ULONG messageId,
	tSynchronizedProcedure procedure,
	ULONG parameter)
{
	tSynchronizedContext SyncContext;
	SyncContext.pContext  = pContext;
	SyncContext.Parameter = parameter;
	return NdisMSynchronizeWithInterruptEx(pContext->InterruptHandle, messageId, procedure, &SyncContext);
}

/**********************************************************
NDIS-required procedure for hardware interrupt registration
Parameters:
	IN PVOID MiniportInterruptContext (actually Adapter context)
***********************************************************/
static VOID MiniportDisableInterruptEx(IN PVOID MiniportInterruptContext)
{
	DEBUG_ENTRY(0);
	ParaNdis_VirtIOEnableIrqSynchronized((PARANDIS_ADAPTER *)MiniportInterruptContext, isAny, FALSE);
}

/**********************************************************
NDIS-required procedure for hardware interrupt registration
Parameters:
	IN PVOID MiniportInterruptContext (actually Adapter context)
***********************************************************/
static VOID MiniportEnableInterruptEx(IN PVOID MiniportInterruptContext)
{
	DEBUG_ENTRY(0);
	ParaNdis_VirtIOEnableIrqSynchronized((PARANDIS_ADAPTER *)MiniportInterruptContext, isAny, TRUE);
}

/**********************************************************
NDIS-required procedure for hardware interrupt handling
Parameters:
    IN PVOID  MiniportInterruptContext (actually Adapter context)
    OUT PBOOLEAN  QueueDefaultInterruptDpc - set to TRUE for default DPC spawning
    OUT PULONG  TargetProcessors
Return value:
	TRUE if recognized
***********************************************************/
static BOOLEAN MiniportInterrupt(
    IN PVOID  MiniportInterruptContext,
    OUT PBOOLEAN  QueueDefaultInterruptDpc,
    OUT PULONG  TargetProcessors
    )
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)MiniportInterruptContext;
	BOOLEAN b;
	b = ParaNdis_OnInterrupt(pContext, QueueDefaultInterruptDpc, isAny);
	*TargetProcessors = 0;
	pContext->ulIrqReceived += b;
	return b;
}

static ULONG MessageToInterruptSource(PARANDIS_ADAPTER *pContext, ULONG  MessageId)
{
	ULONG interruptSource = 0;
	if (MessageId == pContext->ulRxMessage) interruptSource |= isReceive;	
	if (MessageId == pContext->ulTxMessage) interruptSource |= isTransmit;	
	if (MessageId == pContext->ulControlMessage) interruptSource |= isControl;
	return interruptSource;
}

/**********************************************************
NDIS-required procedure for MSI hardware interrupt handling
Parameters:
    IN PVOID  MiniportInterruptContext (actually Adapter context)
    IN ULONG  MessageId - specific interrupt index
    OUT PBOOLEAN  QueueDefaultInterruptDpc - - set to TRUE for default DPC spawning
    OUT PULONG  TargetProcessors
Return value:
	TRUE if recognized
***********************************************************/
static BOOLEAN MiniportMSIInterrupt(
    IN PVOID  MiniportInterruptContext,
    IN ULONG  MessageId,
    OUT PBOOLEAN  QueueDefaultInterruptDpc,
    OUT PULONG  TargetProcessors
    )
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)MiniportInterruptContext;
	BOOLEAN b;
	ULONG interruptSource = MessageToInterruptSource(pContext, MessageId);
	b = ParaNdis_OnInterrupt(pContext, QueueDefaultInterruptDpc, interruptSource);
	pContext->ulIrqReceived += b;
	return b;
}

/**********************************************************
NDIS-required procedure for DPC handling
Parameters:
	PVOID  MiniportInterruptContext (Adapter context)
***********************************************************/
static VOID MiniportInterruptDPC(
    IN PVOID  MiniportInterruptContext,
    IN PVOID  MiniportDpcContext,
    IN PULONG  NdisReserved1,
    IN PULONG  NdisReserved2
    )
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)MiniportInterruptContext;
	ULONG requiresProcessing;
	DEBUG_ENTRY(5);
	requiresProcessing = ParaNdis_DPCWorkBody(pContext);
	if (requiresProcessing)
	{
		DPrintf(4, ("[%s] Queued additional DPC for %d", __FUNCTION__, 	requiresProcessing));
		InterlockedOr(&pContext->InterruptStatus, requiresProcessing);
		NdisMQueueDpc(pContext->InterruptHandle, 0, 1 << KeGetCurrentProcessorNumber(), pContext);
	}
}

/**********************************************************
NDIS-required procedure for MSI DPC handling
Parameters:
	PVOID  MiniportInterruptContext (Adapter context)
    IN ULONG  MessageId - specific interrupt index
***********************************************************/
static VOID MiniportMSIInterruptDpc(
    IN PVOID  MiniportInterruptContext,
    IN ULONG  MessageId,
    IN PVOID  MiniportDpcContext,
    IN PULONG  NdisReserved1,
    IN PULONG  NdisReserved2
    )
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)MiniportInterruptContext;
	ULONG interruptSource = MessageToInterruptSource(pContext, MessageId);
	DPrintf(5, ("[%s] (Message %d, source %d)", __FUNCTION__, MessageId, interruptSource));
	interruptSource = ParaNdis_DPCWorkBody(pContext);
	if (interruptSource)
	{
		DPrintf(4, ("[%s] Queued additional DPC for %d", __FUNCTION__, interruptSource));
		InterlockedOr(&pContext->InterruptStatus, interruptSource);
		NdisMQueueDpc(pContext->InterruptHandle, MessageId, 1 << KeGetCurrentProcessorNumber(), pContext);
	}
}

static VOID MiniportDisableMSIInterrupt(
    IN PVOID  MiniportInterruptContext,
    IN ULONG  MessageId
    )
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)MiniportInterruptContext;
	ULONG interruptSource = MessageToInterruptSource(pContext, MessageId);
	DPrintf(0, ("[%s] (Message %d)", __FUNCTION__, MessageId));
	ParaNdis_VirtIOEnableIrqSynchronized(pContext, interruptSource, FALSE);
}

static VOID MiniportEnableMSIInterrupt(
    IN PVOID  MiniportInterruptContext,
    IN ULONG  MessageId
    )
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)MiniportInterruptContext;
	ULONG interruptSource = MessageToInterruptSource(pContext, MessageId);
	DPrintf(0, ("[%s] (Message %d)", __FUNCTION__, MessageId));
	ParaNdis_VirtIOEnableIrqSynchronized(pContext, interruptSource, TRUE);
}


/**********************************************************
NDIS required handler for run-time allocation of physical memory
Parameters:

Return value:
***********************************************************/
static VOID SharedMemAllocateCompleteHandler(
	IN NDIS_HANDLE  MiniportAdapterContext,
	IN PVOID  VirtualAddress,
	IN PNDIS_PHYSICAL_ADDRESS  PhysicalAddress,
	IN ULONG  Length,
	IN PVOID  Context
	)
{

}

static NDIS_STATUS SetInterruptMessage(PARANDIS_ADAPTER *pContext, UINT queueIndex)
{
	NDIS_STATUS status = NDIS_STATUS_SUCCESS;
	ULONG val;
	ULONG  messageIndex = queueIndex < pContext->pMSIXInfoTable->MessageCount ?
		queueIndex : (pContext->pMSIXInfoTable->MessageCount - 1);
	PULONG pMessage = NULL;
	switch (queueIndex)
	{
	case 0: // Rx queue interrupt:
		WriteVirtIODeviceWord(pContext->IODevice.addr + VIRTIO_PCI_QUEUE_SEL, (u16)queueIndex);
		WriteVirtIODeviceWord(pContext->IODevice.addr + VIRTIO_PCI_CONFIG + 2, (u16)messageIndex);
		val = ReadVirtIODeviceWord(pContext->IODevice.addr + VIRTIO_PCI_CONFIG + 2);
		pMessage = &pContext->ulRxMessage;
		break;
	case 1: // Tx queue interrupt:
		WriteVirtIODeviceWord(pContext->IODevice.addr + VIRTIO_PCI_QUEUE_SEL, (u16)queueIndex);
		WriteVirtIODeviceWord(pContext->IODevice.addr + VIRTIO_PCI_CONFIG + 2, (u16)messageIndex);
		val = ReadVirtIODeviceWord(pContext->IODevice.addr + VIRTIO_PCI_CONFIG + 2);
		pMessage = &pContext->ulTxMessage;
		break;
	case 2: // config interrupt
		WriteVirtIODeviceWord(pContext->IODevice.addr + VIRTIO_PCI_CONFIG, (u16)messageIndex);
		val = ReadVirtIODeviceWord(pContext->IODevice.addr + VIRTIO_PCI_CONFIG);
		pMessage = &pContext->ulControlMessage;
		break;
	default:
		break;
	}

	if (val != messageIndex)
	{
		DPrintf(0, ("[%s] ERROR: Wrong MSI-X message for q%d(w%X,r%X)!", __FUNCTION__, queueIndex, messageIndex, val));
		status = NDIS_STATUS_DEVICE_FAILED;
	}
	if (pMessage) *pMessage = messageIndex;
	return status;
}

static NDIS_STATUS ConfigureMSIXVectors(PARANDIS_ADAPTER *pContext)
{
	NDIS_STATUS status = NDIS_STATUS_RESOURCES;
	UINT i;
	PIO_INTERRUPT_MESSAGE_INFO pTable = pContext->pMSIXInfoTable;
	if (pTable && pTable->MessageCount)
	{
		status = NDIS_STATUS_SUCCESS;
		DPrintf(0, ("[%s] Using MSIX interrupts (%d messages, irql %d)",
			__FUNCTION__, pTable->MessageCount, pTable->UnifiedIrql));
		for (i = 0; i < pContext->pMSIXInfoTable->MessageCount; ++i)
		{
			DPrintf(0, ("[%s] MSIX message%d=%08X=>%I64X",
				__FUNCTION__, i,
				pTable->MessageInfo[i].MessageData,
				pTable->MessageInfo[i].MessageAddress));
		}
		for (i = 0; i < 3 && status == NDIS_STATUS_SUCCESS; ++i)
		{
			status = SetInterruptMessage(pContext, i);
		}
	}
	if (status == NDIS_STATUS_SUCCESS)
	{
		DPrintf(0, ("[%s] Using message %d for RX queue", __FUNCTION__, pContext->ulRxMessage));
		DPrintf(0, ("[%s] Using message %d for TX queue", __FUNCTION__, pContext->ulTxMessage));
		DPrintf(0, ("[%s] Using message %d for controls", __FUNCTION__, pContext->ulControlMessage));
	}
	return status;
}

void ParaNdis_RestoreDeviceConfigurationAfterReset(
	PARANDIS_ADAPTER *pContext)
{
	ConfigureMSIXVectors(pContext);
}


/**********************************************************
NDIS6-related final initialization:
	Installing interrupt handler
	Allocate buffer list pool

Parameters:

Return value:

***********************************************************/
NDIS_STATUS ParaNdis_FinishSpecificInitialization(PARANDIS_ADAPTER *pContext)
{
	NDIS_STATUS status = NDIS_STATUS_SUCCESS;
	NET_BUFFER_LIST_POOL_PARAMETERS PoolParams;
	NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS mic;
	DEBUG_ENTRY(0);

	InitializeListHead(&pContext->WaitingMapping);

	NdisZeroMemory(&mic, sizeof(mic));
	mic.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_INTERRUPT;
	mic.Header.Revision = NDIS_MINIPORT_INTERRUPT_REVISION_1;
	mic.Header.Size = NDIS_SIZEOF_MINIPORT_INTERRUPT_CHARACTERISTICS_REVISION_1;
	mic.DisableInterruptHandler = MiniportDisableInterruptEx;
	mic.EnableInterruptHandler  = MiniportEnableInterruptEx;
	mic.InterruptDpcHandler = MiniportInterruptDPC;
	mic.InterruptHandler = MiniportInterrupt;
	if (pContext->bUsingMSIX)
	{
		mic.MsiSupported = TRUE;
		mic.MsiSyncWithAllMessages = TRUE;
		mic.EnableMessageInterruptHandler = MiniportEnableMSIInterrupt;
		mic.DisableMessageInterruptHandler = MiniportDisableMSIInterrupt;
		mic.MessageInterruptHandler = MiniportMSIInterrupt;
		mic.MessageInterruptDpcHandler = MiniportMSIInterruptDpc;
	}
	PoolParams.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
	PoolParams.Header.Size = sizeof(PoolParams);
	PoolParams.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
	PoolParams.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
	PoolParams.fAllocateNetBuffer = TRUE;
	PoolParams.ContextSize = 0;
	PoolParams.PoolTag = PARANDIS_MEMORY_TAG;
	PoolParams.DataSize = 0;

	pContext->BufferListsPool = NdisAllocateNetBufferListPool(pContext->MiniportHandle, &PoolParams);
	if (!pContext->BufferListsPool)
	{
		status = NDIS_STATUS_RESOURCES;
	}
	if (status == NDIS_STATUS_SUCCESS)
	{
		status = NdisMRegisterInterruptEx(pContext->MiniportHandle, pContext, &mic, &pContext->InterruptHandle);
	}

	if (status == NDIS_STATUS_SUCCESS)
	{
		NDIS_SG_DMA_DESCRIPTION sgDesc;
		sgDesc.Header.Type = NDIS_OBJECT_TYPE_SG_DMA_DESCRIPTION;
		sgDesc.Header.Revision = NDIS_SG_DMA_DESCRIPTION_REVISION_1;
		sgDesc.Header.Size = sizeof(sgDesc);
		sgDesc.Flags = NDIS_SG_DMA_64_BIT_ADDRESS;
		sgDesc.MaximumPhysicalMapping = 0x10000; // 64K
		sgDesc.ProcessSGListHandler = ProcessSGListHandler;
		sgDesc.SharedMemAllocateCompleteHandler = SharedMemAllocateCompleteHandler;
		sgDesc.ScatterGatherListSize = 0; // OUT value
		status = NdisMRegisterScatterGatherDma(pContext->MiniportHandle, &sgDesc, &pContext->DmaHandle);
		if (status != NDIS_STATUS_SUCCESS)
		{
			DPrintf(0, ("[%s] ERROR: NdisMRegisterScatterGatherDma failed (%X)!", __FUNCTION__, status));
		}
		else
		{
			DPrintf(0, ("[%s] SG recommended size %d", __FUNCTION__, sgDesc.ScatterGatherListSize));
		}
	}

	if (status == NDIS_STATUS_SUCCESS)
	{
		if (NDIS_CONNECT_MESSAGE_BASED == mic.InterruptType)
		{
			pContext->pMSIXInfoTable = mic.MessageInfoTable;
			status = ConfigureMSIXVectors(pContext);
			//pContext->bDoInterruptRecovery = FALSE;
		}
		else if (pContext->bUsingMSIX)
		{
			DPrintf(0, ("[%s] ERROR: Interrupt type %d, message table %p",
				__FUNCTION__, mic.InterruptType, mic.MessageInfoTable));
			pContext->bUsingMSIX = FALSE;
		}
		ParaNdis6_ApplyOffloadPersistentConfiguration(pContext);
	}
	DEBUG_EXIT_STATUS(0, status);
	return status;
}

/**********************************************************
NDIS6-related final initialization:
	Uninstalling interrupt handler
	Dellocate buffer list pool
Parameters:
	context
***********************************************************/
VOID ParaNdis_FinalizeCleanup(PARANDIS_ADAPTER *pContext)
{
	// we zero context members to be able examine them in the debugger/dump
	if (pContext->InterruptHandle)
	{
		NdisMDeregisterInterruptEx(pContext->InterruptHandle);
		pContext->InterruptHandle = NULL;
	}
	if (pContext->BufferListsPool)
	{
		NdisFreeNetBufferListPool(pContext->BufferListsPool);
		pContext->BufferListsPool = NULL;
	}
	if (pContext->DmaHandle)
	{
		NdisMDeregisterScatterGatherDma(pContext->DmaHandle);
		pContext->DmaHandle = NULL;
	}
}

static FORCEINLINE ULONG MaxMDLDataSize(PARANDIS_ADAPTER *pContext, pIONetDescriptor pBufferDesc)
{
	ULONG size  = pBufferDesc->DataInfo.size;
	if (pContext->bUseMergedBuffers) size -= pContext->nVirtioHeaderSize;
	return size;
}

/**********************************************************
NDIS6-specific procedure for binding RX buffer to MDL
Parameters:
	context
	pIONetDescriptor pBuffersDesc	VirtIO buffer descriptor

Return value:
	TRUE, if bound successfully
	FALSE, if no buffer or packet can be allocated
***********************************************************/
BOOLEAN ParaNdis_BindBufferToPacket(
	PARANDIS_ADAPTER *pContext,
	pIONetDescriptor pBufferDesc)
{
	PMDL pMDL;
	PVOID pData = RtlOffsetToPointer(pBufferDesc->DataInfo.Virtual,
		pContext->bUseMergedBuffers ? pContext->nVirtioHeaderSize : 0);
	pMDL = NdisAllocateMdl(pContext->MiniportHandle, pData, MaxMDLDataSize(pContext, pBufferDesc));
	pBufferDesc->pHolder = pMDL;
	return pMDL != NULL;
}

/**********************************************************
NDIS5-specific procedure for unbinding previously bound MDL
from it's RX buffer
Parameters:
	context
	pIONetDescriptor pBuffersDesc	VirtIO buffer descriptor
***********************************************************/
void ParaNdis_UnbindBufferFromPacket(
	PARANDIS_ADAPTER *pContext,
	pIONetDescriptor pBufferDesc)
{
	NdisAdjustMdlLength(pBufferDesc->pHolder, MaxMDLDataSize(pContext, pBufferDesc));
	NdisFreeMdl(pBufferDesc->pHolder);
}


/**********************************************************
NDIS6 implementation of packet indication

Parameters:
	context
	PVOID pBuffersDescriptor - VirtIO buffer descriptor of data buffer
	PVOID pData  - data buffer to pass to network stack
	PULONG pLength - size of received packet.
	BOOLEAN bPrepareOnly - only return NBL for further indication in batch
Return value:
	TRUE  is packet indicated
	FALSE if not (in this case, the descriptor should be freed now)
If priority header is in the packet. it will be removed and *pLength decreased
***********************************************************/
tPacketIndicationType ParaNdis_IndicateReceivedPacket(
	PARANDIS_ADAPTER *pContext,
	PVOID dataBuffer,
	PULONG pLength,
	BOOLEAN bPrepareOnly,
	pIONetDescriptor pBuffersDesc)
{
	PMDL pMDL = pBuffersDesc->pHolder;
	ULONG length = *pLength;
	PNET_BUFFER_LIST pNBL = NULL;

	if (pMDL)
	{
		NDIS_NET_BUFFER_LIST_8021Q_INFO qInfo;
		qInfo.Value = NULL;
		if ((pContext->ulPriorityVlanSetting && length > (ETH_HEADER_SIZE + ETH_PRIORITY_HEADER_SIZE)) ||
			length > pContext->MaxPacketSize.nMaxFullSizeOS)
		{
			PUCHAR pPriority = (PUCHAR)dataBuffer + ETH_PRIORITY_HEADER_OFFSET;
			if (ETH_HAS_PRIO_HEADER(dataBuffer))
			{
				if (pContext->ulPriorityVlanSetting & 1)
					qInfo.TagHeader.UserPriority = (pPriority[2] & 0xE0) >> 5;
				if (pContext->ulPriorityVlanSetting & 2)
				{
					qInfo.TagHeader.VlanId = (((USHORT)(pPriority[2] & 0x0F)) << 8) | pPriority[3];
					if (pContext->VlanId && pContext->VlanId != qInfo.TagHeader.VlanId)
					{
						DPrintf(0, ("[%s] Failing unexpected VlanID %d", __FUNCTION__, qInfo.TagHeader.VlanId));
						pMDL = NULL;
					}
				}
				if (1)
				{
					RtlMoveMemory(
						pPriority,
						pPriority + ETH_PRIORITY_HEADER_SIZE,
						length - ETH_PRIORITY_HEADER_OFFSET - ETH_PRIORITY_HEADER_SIZE);
					length -= ETH_PRIORITY_HEADER_SIZE;
					*pLength = length;
					if (length > pContext->MaxPacketSize.nMaxFullSizeOS)
					{
						DPrintf(0, ("[%s] Can not indicate up packet of %d", __FUNCTION__, length));
						pMDL = NULL;
					}
				}
				else
				{
					// todo: avoid data copy.
					// use 2 MDL: 1 for ethernet header, 1 for data
				}
			}
		}
		if (pMDL)
		{
			NdisAdjustMdlLength(pMDL, length);
			pNBL = NdisAllocateNetBufferAndNetBufferList(
				pContext->BufferListsPool,
				0,
				0,
				pMDL,
				0,
				length);
		}
		if (pNBL)
		{
			pNBL->SourceHandle = pContext->MiniportHandle;
			NET_BUFFER_LIST_INFO(pNBL, Ieee8021QNetBufferListInfo) = qInfo.Value;
			if (qInfo.Value)
			{
				DPrintf(1, ("Found priority tag %p", qInfo.Value));
			}
			pNBL->MiniportReserved[0] = pBuffersDesc;
			pNBL->Status = NDIS_STATUS_SUCCESS;
#if defined(ENABLE_HISTORY_LOG)
			{
				tTcpIpPacketParsingResult packetReview = ParaNdis_CheckSumVerify(
					RtlOffsetToPointer(dataBuffer, ETH_HEADER_SIZE),
					length,
					pcrIpChecksum | pcrTcpChecksum | pcrUdpChecksum,
					__FUNCTION__
					);
				ParaNdis_DebugHistory(pContext, hopPacketReceived, pNBL, length, (ULONG)(ULONG_PTR)qInfo.Value, packetReview.value);
			}
#endif
			if (!bPrepareOnly)
			{
				DPrintf(1, ("  Reporting NBL of %d bytes of pBuffersDescriptor %p",
					length, pBuffersDesc));
				NdisMIndicateReceiveNetBufferLists(
					pContext->MiniportHandle, pNBL, 0, 1, 0);
			}
		}
	}
	if (!pNBL)
	{
		DPrintf(0, ("[%s] Error: cannot indicate packet for desc.%p(%d b.)", __FUNCTION__,
			pBuffersDesc, length));
	}
	return pNBL;
}


VOID ParaNdis_IndicateReceivedBatch(
	PARANDIS_ADAPTER *pContext,
	tPacketIndicationType *pBatch,
	ULONG nofPackets)
{
	ULONG i;
	PNET_BUFFER_LIST pPrev = pBatch[0];
	NET_BUFFER_LIST_NEXT_NBL(pPrev) = NULL;
	for (i = 1; i < nofPackets; ++i)
	{
		PNET_BUFFER_LIST pNBL = pBatch[i];
		NET_BUFFER_LIST_NEXT_NBL(pPrev) = pNBL;
		NET_BUFFER_LIST_NEXT_NBL(pNBL)  = NULL;
		pPrev = pNBL;
	}
	NdisMIndicateReceiveNetBufferLists(
		pContext->MiniportHandle,
		pBatch[0],
		0,
		nofPackets,
		0);

}


/**********************************************************
NDIS procedure of returning us buffer of previously indicated packets
Parameters:
	context
	PNET_BUFFER_LIST pNBL - list of buffers to free
	returnFlags - is dpc

The procedure frees:
received buffer descriptors back to list of RX buffers
all the allocated MDL structures
all the received NBLs back to our pool
***********************************************************/
VOID ParaNdis6_ReturnNetBufferLists(
	NDIS_HANDLE miniportAdapterContext,
	PNET_BUFFER_LIST pNBL, ULONG returnFlags)
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)miniportAdapterContext;
	DEBUG_ENTRY(5);
	while (pNBL)
	{
		PNET_BUFFER_LIST pTemp = pNBL;
		pIONetDescriptor pBuffersDescriptor = (pIONetDescriptor)pNBL->MiniportReserved[0];
		DPrintf(3, ("  Returned NBL of pBuffersDescriptor %p!", pBuffersDescriptor));
		pNBL = NET_BUFFER_LIST_NEXT_NBL(pNBL);
		NET_BUFFER_LIST_NEXT_NBL(pTemp) = NULL;
		NdisFreeNetBufferList(pTemp);
		NdisAcquireSpinLock(&pContext->ReceiveLock);
		ParaNdis_VirtIONetReuseRecvBuffer(pContext, pBuffersDescriptor);
		NdisReleaseSpinLock(&pContext->ReceiveLock);
	}
}

/**********************************************************
Pauses of restarts RX activity.
Restart is immediate, pause may be delayed until
NDIS returns all the indicated NBL

Parameters:
	context
	bPause 1/0 - pause or restart
	ONPAUSECOMPLETEPROC Callback to be called when PAUSE finished
Return value:
	SUCCESS if finished synchronously
	PENDING if not, then callback will be called
***********************************************************/
NDIS_STATUS ParaNdis6_ReceivePauseRestart(
	PARANDIS_ADAPTER *pContext,
	BOOLEAN bPause,
	ONPAUSECOMPLETEPROC Callback
	)
{
	NDIS_STATUS status = NDIS_STATUS_SUCCESS;
	NdisAcquireSpinLock(&pContext->ReceiveLock);
	if (bPause)
	{
		ParaNdis_DebugHistory(pContext, hopInternalReceivePause, NULL, 1, 0, 0);
		if (!IsListEmpty(&pContext->NetReceiveBuffersWaiting))
		{
			pContext->ReceiveState = srsPausing;
			pContext->ReceivePauseCompletionProc = Callback;
			status = NDIS_STATUS_PENDING;
		}
		else
		{
			ParaNdis_DebugHistory(pContext, hopInternalReceivePause, NULL, 0, 0, 0);
			pContext->ReceiveState = srsDisabled;
		}
	}
	else
	{
		ParaNdis_DebugHistory(pContext, hopInternalReceiveResume, NULL, 0, 0, 0);
		pContext->ReceiveState = srsEnabled;
	}
	NdisReleaseSpinLock(&pContext->ReceiveLock);
	return status;
}

/**********************************************************
Copies single packet (MDL list) to the buffer (at least 60 bytes)
Parameters:
	PNET_BUFFER pB - packet
	PVOID dest     - where to copy
	ULONG maxSize  - max size of destination buffer
Return value:
	number of bytes copied
	0 on error
***********************************************************/
tCopyPacketResult ParaNdis_PacketCopier(PNET_BUFFER pB, PVOID dest, ULONG maxSize, PVOID refValue, BOOLEAN bPreview)
{
	tCopyPacketResult result;
	ULONG PriorityDataLong = 0;
	ULONG nCopied = 0;
	ULONG ulOffset = NET_BUFFER_CURRENT_MDL_OFFSET(pB);
	ULONG nToCopy = NET_BUFFER_DATA_LENGTH(pB);
	PMDL  pMDL = NET_BUFFER_CURRENT_MDL(pB);
	result.error = cpeOK;
	if (!bPreview) PriorityDataLong = ((tNetBufferListEntry *)(((tNetBufferEntry *)refValue)->nbl->Scratch))->PriorityDataLong;
	if (nToCopy > maxSize) nToCopy = bPreview ? maxSize : 0;

	while (pMDL && nToCopy)
	{
		ULONG len;
		PVOID addr;
		NdisQueryMdl(pMDL, &addr, &len, NormalPagePriority);
		if (addr && len)
		{
			// total to copy from this MDL
			len -= ulOffset;
			if (len > nToCopy) len = nToCopy;
			nToCopy -= len;
			if ((PriorityDataLong & 0xFFFF) &&
				nCopied < ETH_PRIORITY_HEADER_OFFSET &&
				(nCopied + len) >= ETH_PRIORITY_HEADER_OFFSET)
			{
				ULONG nCopyNow = ETH_PRIORITY_HEADER_OFFSET - nCopied;
				NdisMoveMemory(dest, (PCHAR)addr + ulOffset, nCopyNow);
				dest = (PCHAR)dest + nCopyNow;
				addr = (PCHAR)addr + nCopyNow;
				NdisMoveMemory(dest, &PriorityDataLong, ETH_PRIORITY_HEADER_SIZE);
				nCopied += ETH_PRIORITY_HEADER_SIZE;
				dest = (PCHAR)dest + ETH_PRIORITY_HEADER_SIZE;
				nCopyNow = len - nCopyNow;
				if (nCopyNow) NdisMoveMemory(dest, (PCHAR)addr + ulOffset, nCopyNow);
				dest = (PCHAR)dest + nCopyNow;
				ulOffset = 0;
				nCopied += len;
			}
			else
			{
				NdisMoveMemory(dest, (PCHAR)addr + ulOffset, len);
				dest = (PCHAR)dest + len;
				ulOffset = 0;
				nCopied += len;
			}
		}
		pMDL = pMDL->Next;
	}

	DEBUG_EXIT_STATUS(4, nCopied);
	result.size = nCopied;
	return result;
}


static FORCEINLINE ULONG CalculateTotalOffloadSize(
	ULONG packetSize,
	ULONG mss,
	ULONG ipheaderOffset,
	ULONG maxPacketSize,
	tTcpIpPacketParsingResult packetReview)
{
	ULONG ul = 0;
	ULONG tcpipHeaders = packetReview.XxpIpHeaderSize;
	ULONG allHeaders = tcpipHeaders + ipheaderOffset;
#if 1
	if (tcpipHeaders && (mss + allHeaders) <= maxPacketSize)
	{
		ul = packetSize - allHeaders;
	}
	DPrintf(1, ("[%s]%s %d/%d, headers %d)", __FUNCTION__, !ul ? "ERROR:" : "", ul, mss, allHeaders));
#else
	UINT  calculationType = 3;
	if (tcpipHeaders && (mss + allHeaders) <= maxPacketSize)
	{
		ULONG nFragments = (packetSize - allHeaders)/mss;
		ULONG last = (packetSize - allHeaders)%mss;
		ULONG tcpHeader = tcpipHeaders - packetReview.ipHeaderSize;
		switch (calculationType)
		{
			case 0:
				ul = nFragments * (mss + allHeaders) + last + (last ? allHeaders : 0);
				break;
			case 1:
				ul = nFragments * (mss + tcpipHeaders) + last + (last ? tcpipHeaders : 0);
				break;
			case 2:
				ul = nFragments * (mss + tcpHeader) + last + (last ? tcpHeader : 0);
				break;
			case 3:
				ul = packetSize - allHeaders;
				break;
			case 4:
				ul = packetSize - ETH_HEADER_SIZE;
				break;
			case 5:
				ul = packetSize - ipheaderOffset;
				break;
			default:
				break;
		}
	}
	DPrintf(1, ("[%s:%d]%s %d/%d, headers %d)",
		__FUNCTION__, calculationType, !ul ? "ERROR:" : "", ul, mss, allHeaders));
#endif
	return ul;
}


tMapperResult ParaNdis_PacketMapper(
	PARANDIS_ADAPTER *pContext,
	tPacketType packet,
	PVOID ReferenceValue,
	struct VirtIOBufferDescriptor *buffers,
	pIONetDescriptor pDesc
	)
{
	tMapperResult res;
	tNetBufferEntry *pnbe = (tNetBufferEntry *)ReferenceValue;
	res.ulDataSize = 0;
	res.nBuffersMapped = 0;
	if (pnbe->netBuffer == packet)
	{
		PSCATTER_GATHER_LIST pSGList = pnbe->pSGList;
		if (pSGList)
		{
			UINT i, lengthGet = 0, lengthPut = 0;
			SCATTER_GATHER_ELEMENT *pSGElements = pSGList->Elements;
			tNetBufferListEntry *pble = (tNetBufferListEntry *)pnbe->nbl->Scratch;
			UINT nCompleteBuffersToSkip = 0;
			UINT nBytesSkipInFirstBuffer = NET_BUFFER_CURRENT_MDL_OFFSET(packet);
			ULONG PriorityDataLong = pble->PriorityDataLong;
			if (pble->mss)
			{
				lengthGet = pble->tcpHeaderOffset + sizeof(TCPHeader);
			}
			if (PriorityDataLong && !lengthGet)
			{
				lengthGet = ETH_HEADER_SIZE;
			}
			if (lengthGet)
			{
				ULONG len = 0;
				for (i = 0; i < pSGList->NumberOfElements; ++i)
				{
					len += pSGList->Elements[i].Length - nBytesSkipInFirstBuffer;
					DPrintf(2, ("[%s] buffer %d of %d->%d",
						__FUNCTION__, nCompleteBuffersToSkip, pSGElements[i].Length, len));
					if (len > lengthGet)
					{
						nBytesSkipInFirstBuffer = pSGList->Elements[i].Length - (len - lengthGet);
						break;
					}
					nCompleteBuffersToSkip++;
					nBytesSkipInFirstBuffer = 0;
				}
				lengthPut = lengthGet + (PriorityDataLong ? ETH_PRIORITY_HEADER_SIZE : 0);
			}

			if (lengthPut > pDesc->DataInfo.size)
			{
				DPrintf(0, ("[%s] ERROR: can not substitute %d bytes, sending as is", __FUNCTION__, lengthPut));
				nCompleteBuffersToSkip = 0;
				lengthPut = lengthGet = 0;
				nBytesSkipInFirstBuffer = NET_BUFFER_CURRENT_MDL_OFFSET(packet);
			}

			if (lengthPut)
			{
				// we replace 1 or more HW buffers with one buffer preallocated for data
				buffers->physAddr = pDesc->DataInfo.Physical;
				buffers->ulSize   = lengthPut;
				res.ulDataSize += lengthGet;
				res.nBuffersMapped = pSGList->NumberOfElements - nCompleteBuffersToSkip + 1;
				pSGElements += nCompleteBuffersToSkip;
				buffers++;
				DPrintf(1, ("[%s] (%d bufs) skip %d buffers + %d bytes",
					__FUNCTION__, pSGList->NumberOfElements, nCompleteBuffersToSkip, nBytesSkipInFirstBuffer));
			}
			else
			{
				res.nBuffersMapped = pSGList->NumberOfElements;
			}

			for (i = nCompleteBuffersToSkip; i < pSGList->NumberOfElements; ++i)
			{
				if (nBytesSkipInFirstBuffer)
				{
					buffers->physAddr.QuadPart = pSGElements->Address.QuadPart + nBytesSkipInFirstBuffer;
					buffers->ulSize   = pSGElements->Length - nBytesSkipInFirstBuffer;
					DPrintf(2, ("[%s] using HW buffer %d of %d-%d", __FUNCTION__, i, pSGElements->Length, nBytesSkipInFirstBuffer));
					nBytesSkipInFirstBuffer = 0;
				}
				else
				{
					buffers->physAddr = pSGElements->Address;
					buffers->ulSize   = pSGElements->Length;
				}
				res.ulDataSize += buffers->ulSize;
				pSGElements++;
				buffers++;
			}

			if (lengthPut)
			{
				PVOID pBuffer = pDesc->DataInfo.Virtual;
				ParaNdis_PacketCopier(packet, pBuffer, lengthGet, ReferenceValue, TRUE);
				if (pble->mss)
				{
					tTcpIpPacketParsingResult packetReview;
					NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO lso;
					ULONG dummyTransferSize;
					ULONG flags = pcrIpChecksum | pcrTcpChecksum | pcrFixPHChecksum;
					ULONG saveBuffers = res.nBuffersMapped;
					PVOID pIpHeader = RtlOffsetToPointer(pBuffer, pContext->Offload.ipHeaderOffset);
					res.nBuffersMapped = 0;
					if (pContext->bFixIPChecksum) flags |= pcrFixIPChecksum;
					packetReview = ParaNdis_CheckSumVerify(
						pIpHeader,
						lengthGet - pContext->Offload.ipHeaderOffset,
						flags,
						__FUNCTION__);
					if (packetReview.ipCheckSum == ppresCSOK || packetReview.fixedIpCS)
					{
						dummyTransferSize =	CalculateTotalOffloadSize(
							res.ulDataSize,
							pble->mss,
							pContext->Offload.ipHeaderOffset,
							pContext->MaxPacketSize.nMaxFullSizeOS,
							packetReview);
						if (packetReview.xxpStatus == ppresXxpIncomplete)
						{
							DPrintf(0, ("[%s] CHECK: IPHO %d, TCPHO %d, IPHS %d, XXPHS %d", __FUNCTION__,
								pContext->Offload.ipHeaderOffset,
								pble->tcpHeaderOffset,
								packetReview.ipHeaderSize,
								packetReview.XxpIpHeaderSize
								));

						}
					}
					else
					{
						DPrintf(0, ("[%s] ERROR locating IP header in %d bytes(IP header of %d)", __FUNCTION__,
							lengthGet, packetReview.ipHeaderSize));
					}
					lso.Value = NET_BUFFER_LIST_INFO(pnbe->nbl, TcpLargeSendNetBufferListInfo);
					if (lso.LsoV1TransmitComplete.Type == NDIS_TCP_LARGE_SEND_OFFLOAD_V1_TYPE)
					{
						lso.LsoV1TransmitComplete.TcpPayload += dummyTransferSize;
						NET_BUFFER_LIST_INFO(pnbe->nbl, TcpLargeSendNetBufferListInfo) = lso.Value;
					}
					if (dummyTransferSize)
					{
						virtio_net_hdr_basic *pheader = pDesc->HeaderInfo.Virtual;
						pheader->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
						pheader->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
						pheader->hdr_len  = (USHORT)(packetReview.XxpIpHeaderSize + pContext->Offload.ipHeaderOffset);
						pheader->gso_size = (USHORT)pble->mss;
						pheader->csum_start = (USHORT)pble->tcpHeaderOffset;
						pheader->csum_offset = TCP_CHECKSUM_OFFSET;
						res.nBuffersMapped = saveBuffers;
					}
				}

				if (PriorityDataLong && res.nBuffersMapped)
				{
					RtlMoveMemory(
						RtlOffsetToPointer(pBuffer, ETH_PRIORITY_HEADER_OFFSET + ETH_PRIORITY_HEADER_SIZE),
						RtlOffsetToPointer(pBuffer, ETH_PRIORITY_HEADER_OFFSET),
						lengthGet - ETH_PRIORITY_HEADER_OFFSET
						);
					NdisMoveMemory(
						RtlOffsetToPointer(pBuffer, ETH_PRIORITY_HEADER_OFFSET),
						&PriorityDataLong,
						sizeof(ETH_PRIORITY_HEADER_SIZE));
					DPrintf(1, ("[%s] Populated priority value %lX", __FUNCTION__, PriorityDataLong));
				}
			}
		}
		else
		{
			DPrintf(0, ("[%s] ERROR: packet (nbe %p) is not mapped!", __FUNCTION__, pnbe));
		}
	}
	else
	{
		DPrintf(0, ("[%s] ERROR: packet <> NBE!", __FUNCTION__));
	}
	return res;
}

static void FreeAllocatedNBLResources(PARANDIS_ADAPTER *pContext, PNET_BUFFER_LIST pNBL)
{
	tNetBufferListEntry	*pble = (tNetBufferListEntry *)pNBL->Scratch;
	pNBL->Scratch = NULL;
	if (pble)
	{
		while (!IsListEmpty(&pble->bufferEntries))
		{
			tNetBufferEntry *pnbe = (tNetBufferEntry *)RemoveHeadList(&pble->bufferEntries);
			if (pnbe->pSGList)
			{
				NdisMFreeNetBufferSGList(pContext->DmaHandle, pnbe->pSGList, pnbe->netBuffer);
			}
			NdisFreeMemory(pnbe, 0, 0);
		}
		NdisFreeMemory(pble, 0, 0);
	}
}

static __inline void ParseSingleNBL(PNET_BUFFER_LIST pNBL, tNBLDigest *pDigest)
{
	ULONG nBuffers;
	ULONG nBytes;
	PNET_BUFFER pB = NET_BUFFER_LIST_FIRST_NB(pNBL);
	nBuffers = nBytes = 0;
	while (pB)
	{
		nBuffers++;
		nBytes += NET_BUFFER_DATA_LENGTH(pB);
		pB = NET_BUFFER_NEXT_NB(pB);
	}

	pDigest->nLists = 1;
	pDigest->nBuffers = nBuffers;
	pDigest->nBytes = nBytes;
}

/* count lists, buffers and bytes in NBL for statistics */
static void __inline ParseNBL(PNET_BUFFER_LIST pNBL, tNBLDigest *pDigest)
{
	tNBLDigest oneDigest;
	pDigest->nLists = 0;
	pDigest->nBuffers = 0;
	pDigest->nBytes = 0;
	while (pNBL)
	{
		pDigest->nLists++;
		ParseSingleNBL(pNBL, &oneDigest);
		pNBL = NET_BUFFER_LIST_NEXT_NBL(pNBL);
		pDigest->nBuffers += oneDigest.nBuffers;
		pDigest->nBytes += oneDigest.nBytes;
	}
}

/**********************************************************
Return NBL (list) to NDIS with specified status
Locks must NOT be acquired
***********************************************************/
static void CompleteBufferLists(
	PARANDIS_ADAPTER *pContext,
	PNET_BUFFER_LIST pNBL,
	NDIS_STATUS status,
	BOOLEAN IsDpc)
{
	tNBLDigest Digest;
	BOOLEAN bPassive = !IsDpc && (KeGetCurrentIrql() < DISPATCH_LEVEL);
	KIRQL irql;
	PNET_BUFFER_LIST pTemp = pNBL;
	DEBUG_ENTRY(4);
	ParseNBL(pNBL, &Digest);
	DPrintf(2, ("[%s] L%d, B%d, b%d with (%08lX)", __FUNCTION__, Digest.nLists, Digest.nBuffers, Digest.nBytes, status));
	while (pTemp)
	{
		LONG lRestToReturn = NdisInterlockedDecrement(&pContext->NetTxPacketsToReturn);
		if (bPassive) irql = KeRaiseIrqlToDpcLevel();
		FreeAllocatedNBLResources(pContext, pTemp);
		if (bPassive) KeLowerIrql(irql);
		pTemp->Status = status;
		ParaNdis_DebugHistory(pContext, hopSendComplete, pTemp, 0, lRestToReturn, status);
		pTemp = NET_BUFFER_LIST_NEXT_NBL(pTemp);
	}
	NdisMSendNetBufferListsComplete(pContext->MiniportHandle,
			pNBL,
			IsDpc ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0
			);
}



/**********************************************************
Must be called with &pContext->SendLock held

Parameters:
	PNET_BUFFER_LIST pNBL - lists to find tail in
Return value:
	PNET_BUFFER_LIST - last list
***********************************************************/
static PNET_BUFFER_LIST GetTail(PNET_BUFFER_LIST pNBL)
{
	if (!pNBL) return NULL;
	while (NET_BUFFER_LIST_NEXT_NBL(pNBL))
	{
		pNBL = NET_BUFFER_LIST_NEXT_NBL(pNBL);
	}
	return pNBL;
}



/*********************************************************************
Prepares single NBL to be mapped and sent:
Allocate per-NBL entry and save it in NBL->Scratch
Allocate pre-NET_BUFFER entries for each NET_BUFFER and chain them in the list
If some allocation fails, this single NBL will be completed later
with erroneous status and all the allocated resources freed
*********************************************************************/
static BOOLEAN PrepareSingleNBL(
	PARANDIS_ADAPTER *pContext,
	PNET_BUFFER_LIST pNBL)
{
	BOOLEAN bOK = TRUE;
	BOOLEAN bExpectedLSO = FALSE;
	ULONG maxDataLength = 0;
	const char *pFailReason = "Unknown";
	NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO lso;
	PNET_BUFFER pB = NET_BUFFER_LIST_FIRST_NB(pNBL);
	tNetBufferListEntry	*pble = ParaNdis_AllocateMemory(pContext, sizeof(*pble));
	pNBL->Scratch = pble;
	DPrintf(4, ("[%s] NBL %p, NBLE %p", __FUNCTION__, pNBL, pble));
	if (pble)
	{
		NDIS_NET_BUFFER_LIST_8021Q_INFO priorityInfo;
		NdisZeroMemory(pble, sizeof(*pble));
		InitializeListHead(&pble->bufferEntries);
		pble->nbl = pNBL;
		priorityInfo.Value = pContext->ulPriorityVlanSetting ?
			NET_BUFFER_LIST_INFO(pNBL, Ieee8021QNetBufferListInfo) : NULL;
		if (!priorityInfo.TagHeader.VlanId) priorityInfo.TagHeader.VlanId = pContext->VlanId;
		if (priorityInfo.TagHeader.CanonicalFormatId || !IsValidVlanId(pContext, priorityInfo.TagHeader.VlanId))
		{
			bOK = FALSE;
			DPrintf(0, ("[%s] Discarded invalid priority tag %p", __FUNCTION__, priorityInfo.Value));
		}
		else if (priorityInfo.Value)
		{
			// ignore priority, if configured
			if (~pContext->ulPriorityVlanSetting & 1)
				priorityInfo.TagHeader.UserPriority = 0;
			// ignore VlanId, if specified
			if (~pContext->ulPriorityVlanSetting & 2)
				priorityInfo.TagHeader.VlanId = 0;
			if (priorityInfo.Value)
			{
				SetPriorityData(pble->PriorityData, priorityInfo.TagHeader.UserPriority, priorityInfo.TagHeader.VlanId);
				DPrintf(0, ("[%s] Populated priority tag %p", __FUNCTION__, priorityInfo.Value));
			}
		}
	}
	else
	{
		bOK = FALSE;
		pFailReason = "Failure to allocate BLE";
	}
	
	if (bOK && !pB)
	{
		bOK = FALSE;
		pFailReason = "Empty NBL";
	}
	
	while (pB && bOK)
	{
		ULONG dataLength = NET_BUFFER_DATA_LENGTH(pB);
		tNetBufferEntry *pnbe = (tNetBufferEntry *)ParaNdis_AllocateMemory(pContext, sizeof(*pnbe));
		DPrintf(4, ("[%s] NBE %p(nb %p)", __FUNCTION__, pnbe, pB));
		if (pnbe)
		{
			NdisZeroMemory(pnbe, sizeof(*pnbe));
			pnbe->nbl = pNBL;
			pnbe->netBuffer = pB;
			pnbe->pContext = pContext;
			InsertTailList(&pble->bufferEntries, &pnbe->list);
			pble->nBuffers++;
			if (!dataLength)
			{
				bOK = FALSE;
				pFailReason = "zero-length buffer";
			}
			if (maxDataLength < dataLength) maxDataLength = dataLength;
		}
		else
		{
			bOK = FALSE;
			pFailReason = "Failure to allocate NBE";
		}
		pB = NET_BUFFER_NEXT_NB(pB);
	}

	if (bOK)
	{
		if (maxDataLength > pContext->MaxPacketSize.nMaxFullSizeOS) bExpectedLSO = TRUE;
		if (maxDataLength > 0xFFF0)
		{
			bOK = FALSE;
			pFailReason = "too large packet";
		}
		lso.Value = NET_BUFFER_LIST_INFO(pNBL, TcpLargeSendNetBufferListInfo);
		if (lso.Value)
		{
			pble->mss = (USHORT)lso.LsoV2Transmit.MSS;
			pble->tcpHeaderOffset = (USHORT)lso.LsoV2Transmit.TcpHeaderOffset;
			if (lso.LsoV1Transmit.Type != NDIS_TCP_LARGE_SEND_OFFLOAD_V1_TYPE &&
				lso.LsoV2Transmit.Type != NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE)
			{
				bOK = FALSE;
				pFailReason = "wrong LSO transmit type";
			}

			if (lso.LsoV2Transmit.Type == NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE &&
				lso.LsoV2Transmit.IPVersion != NDIS_TCP_LARGE_SEND_OFFLOAD_IPv4)
			{
				bOK = FALSE;
				pFailReason = "IPV6 LSO not supported";
			}

			if (bExpectedLSO &&
				(!lso.LsoV2Transmit.MSS ||
				!lso.LsoV2Transmit.TcpHeaderOffset
				))
			{
				bOK = FALSE;
				pFailReason = "wrong LSO parameters";
			}
			if (!lso.LsoV2Transmit.MSS != !lso.LsoV1Transmit.TcpHeaderOffset)
			{
				bOK = FALSE;
				pFailReason = "inconsistent LSO parameters";
			}
			if (!pContext->Offload.flags.fTxLso || !pContext->bOffloadEnabled)
			{
				bOK = FALSE;
				pFailReason = "LSO request when LSO is off";
			}

			// do it for both LsoV1 and LsoV2
			if (bOK)
			{
				lso.LsoV1TransmitComplete.TcpPayload = 0;
				NET_BUFFER_LIST_INFO(pNBL, TcpLargeSendNetBufferListInfo) = lso.Value;
			}
		}
	}
	if (!bOK)
	{
		DPrintf(0, ("[%s] Failed to prepare NBL %p due to %s", __FUNCTION__, pNBL, pFailReason));
	}
	return bOK;
}

/*********************************************************************
*********************************************************************/
static void StartTransferSingleNBL(PARANDIS_ADAPTER *pContext, PNET_BUFFER_LIST pNBL)
{
	tNetBufferListEntry *pble = (tNetBufferListEntry *)pNBL->Scratch;
	LIST_ENTRY list;
	KIRQL irql;
	BOOLEAN bPassive = KeGetCurrentIrql() < DISPATCH_LEVEL;
	DPrintf(4, ("[%s] NBL %p(pble %p)", __FUNCTION__, pNBL, pble));
	InitializeListHead(&list);
	while (!IsListEmpty(&pble->bufferEntries))
	{
		LIST_ENTRY *ple = RemoveHeadList(&pble->bufferEntries);
		InsertTailList(&list, ple);
	}
	while (!IsListEmpty(&list))
	{
		NDIS_STATUS status;
		tNetBufferEntry *pnbe = (tNetBufferEntry *)RemoveHeadList(&list);
		DPrintf(4, ("[%s] mapping entry %p", __FUNCTION__, pnbe));
		//ParaNdis_DebugHistory(pContext, hopSendPacketRequest, pNBL, 0, 0, status);
		NdisInterlockedInsertTailList(&pContext->WaitingMapping, &pnbe->list, &pContext->SendLock);

		if (bPassive) irql = KeRaiseIrqlToDpcLevel();
		if (pContext->bUseScatterGather)
		{
			status = NdisMAllocateNetBufferSGList(
				pnbe->pContext->DmaHandle,
				pnbe->netBuffer,
				pnbe,
				NDIS_SG_LIST_WRITE_TO_DEVICE,
				NULL,
				0);
			if (status != NDIS_STATUS_SUCCESS)
			{
				((tNetBufferListEntry *)pnbe->nbl->Scratch)->flags |= NBLEFLAGS_FAILED;
				ProcessSGListHandler(NULL, NULL, NULL, pnbe);
			}
		}
		else
		{
			ProcessSGListHandler(NULL, NULL, NULL, pnbe);
		}
		if (bPassive) KeLowerIrql(irql);
	}
}

/**********************************************************
Inserts received lists to internal queue and spawns Tx process procedure
Parameters:
	context
	BOOLEAN IsDpc		NDIS wants it
***********************************************************/
VOID ParaNdis6_Send(
	PARANDIS_ADAPTER *pContext,
	PNET_BUFFER_LIST pNBL,
	BOOLEAN IsDpc)
{
	ULONG i;
	tNBLDigest Digest;
	PNET_BUFFER_LIST nextList;
	/* calculate nofLists, nofBuffer and nofBytes for logging */
	ParseNBL(pNBL, &Digest);
	DPrintf(1, (" Send request L%d, B%d, b%d", Digest.nLists, Digest.nBuffers, Digest.nBytes));
	ParaNdis_DebugHistory(pContext, hopSend, pNBL, Digest.nLists, Digest.nBuffers, Digest.nBytes);

	for (i = 0; i < Digest.nLists; ++i)
	{
		NdisInterlockedIncrement(&pContext->NetTxPacketsToReturn);
	}

	nextList = pNBL;
	while (nextList)
	{
		BOOLEAN bOK;
		PNET_BUFFER_LIST temp;
		bOK = PrepareSingleNBL(pContext, nextList);
		temp = nextList;
		nextList = NET_BUFFER_LIST_NEXT_NBL(nextList);
		NET_BUFFER_LIST_NEXT_NBL(temp) = NULL;

		if (bOK && !pContext->bSurprizeRemoved && pContext->bConnected && pContext->SendState == srsEnabled)
		{
			ParaNdis_DebugHistory(pContext, hopSendNBLRequest, temp, NUMBER_OF_PACKETS_IN_NBL(temp), 0, 0);
			StartTransferSingleNBL(pContext, temp);
		}
		else
		{
			NDIS_STATUS status = NDIS_STATUS_FAILURE;
			if (pContext->SendState != srsEnabled) status = NDIS_STATUS_PAUSED;
			if (!pContext->bConnected) status = NDIS_STATUS_MEDIA_DISCONNECTED;
			if (pContext->bSurprizeRemoved) status = NDIS_STATUS_NOT_ACCEPTED;
			CompleteBufferLists(pContext, temp, status, IsDpc);
		}
	}
}

/**********************************************************
	Must be called with SendLock held
***********************************************************/
static void OnNetBufferEntryCompleted(tNetBufferEntry *pnbe)
{
	tNetBufferListEntry *pble = (tNetBufferListEntry *)pnbe->nbl->Scratch;
	pble->nBuffersDone++;
	pble->nBuffersWaiting--;
	DPrintf(3, ("[%s] pble %p, nbe %p", __FUNCTION__, pble, pnbe));
	ParaNdis_DebugHistory(pnbe->pContext, hopBufferSent, pble->nbl, pble->nBuffersDone,
		pnbe->pContext->nofFreeHardwareBuffers, pnbe->pContext->nofFreeTxDescriptors);
	if (pnbe->pSGList)
	{
		NdisMFreeNetBufferSGList(pnbe->pContext->DmaHandle, pnbe->pSGList, pnbe->netBuffer);
	}
	NdisFreeMemory(pnbe, 0, 0);
}

/**********************************************************
	Callback on finished Tx descriptor
	called with SendLock held
***********************************************************/
VOID ParaNdis_OnTransmitBufferReleased(PARANDIS_ADAPTER *pContext, IONetDescriptor *pDesc)
{
	tNetBufferEntry *pnbe = pDesc->ReferenceValue;
	pDesc->ReferenceValue = NULL;
	if (pnbe)
	{
		OnNetBufferEntryCompleted(pnbe);
	}
	else
	{
		ParaNdis_DebugHistory(pContext, hopBufferSent, NULL, 0, pContext->nofFreeHardwareBuffers, pContext->nofFreeTxDescriptors);
		DPrintf(0, ("[%s] ERROR: Send Entry (NBE) not set!", __FUNCTION__));
	}
}

/**********************************************************
NDIS required handler for run-time allocation of scatter-gather list
Parameters:
pSGL - scatter-hather list of elements (possible NULL when called directly)
Context - (tNetBufferEntry *) for specific NET_BUFFER in NBL
Called on DPC (DDK claims it)
***********************************************************/
VOID ProcessSGListHandler(
    IN PDEVICE_OBJECT  pDO,
    IN PVOID  Reserved,
    IN PSCATTER_GATHER_LIST  pSGL,
    IN PVOID  Context
    )
{
	tNetBufferEntry *pnbe = (tNetBufferEntry *)Context;
	PARANDIS_ADAPTER *pContext = pnbe->pContext;
	PNET_BUFFER_LIST pNBL = pnbe->nbl;
	LONG DoneCounter;
	tNetBufferListEntry *pble = (tNetBufferListEntry *)pNBL->Scratch;

	NdisAcquireSpinLock(&pContext->SendLock);
	// remove the netbuffer entry from WaitingMapping list
	RemoveEntryList(&pnbe->list);
	// insert it into list of buffers under netbufferlist entry
	InsertTailList(&pble->bufferEntries, &pnbe->list);
	NdisReleaseSpinLock(&pContext->SendLock);

	pnbe->pSGList = pSGL;
	DoneCounter = InterlockedIncrement(&pble->nBuffersMapped);
	DPrintf(3, ("[%s] mapped %d of %d(%d)", __FUNCTION__,
		pble->nBuffersMapped,
		pble->nBuffers,
		NdisQueryNetBufferPhysicalCount(pnbe->netBuffer)));
	ParaNdis_DebugHistory(pContext, hopSendPacketMapped, pNBL, DoneCounter, pSGL ? pSGL->NumberOfElements : 0, 0);
	if (DoneCounter == pble->nBuffers)
	{
		if (~pble->flags & NBLEFLAGS_FAILED)
		{
			// all buffers are mapped (or failed mapping)
			// we can insert the NBL into send queue and start sending
			NdisAcquireSpinLock(&pContext->SendLock);
			//check consistency: only both head and tail could be NULL
			if (pContext->SendHead && !pContext->SendTail)
			{
				DPrintf(0, ("[%s] ERROR: SendTail not found!", __FUNCTION__));
				pContext->SendTail = GetTail(pContext->SendHead);
			}

			if (pContext->SendTail)
			{
				NET_BUFFER_LIST_NEXT_NBL(pContext->SendTail) = pNBL;
				pContext->SendTail = pNBL;
			}
			else
			{
				pContext->SendHead = pNBL;
				pContext->SendTail = pNBL;
			}
			NdisReleaseSpinLock(&pContext->SendLock);
			// start sending. we are on DPC
			ParaNdis_ProcessTx(pContext, TRUE);
		}
		else
		{
			// some or all buffers are not mapped,
			// complete the entire NBL as failed (we are on DPC)
			CompleteBufferLists(pContext, pNBL, NDIS_STATUS_FAILURE, TRUE);
		}
	}
}

/**********************************************************
Removes specific NBL from list started at SendHead
Must be called with &pContext->SendLock held
Parameters:
	Context
	NBL to remove
***********************************************************/
static void RemoveNBL(PARANDIS_ADAPTER *pContext, PNET_BUFFER_LIST pNBL)
{
	PNET_BUFFER_LIST Removed = NULL;
	if (!pNBL) return;
	if (pNBL == pContext->SendHead)
	{
		pContext->SendHead = NET_BUFFER_LIST_NEXT_NBL(pNBL);
		Removed = pNBL;
	}
	else
	{
		PNET_BUFFER_LIST pPrev = pContext->SendHead;
		while(pPrev)
		{
			PNET_BUFFER_LIST pNext = NET_BUFFER_LIST_NEXT_NBL(pPrev);
			if (pNext == pNBL)
			{
				/* remove it */
				NET_BUFFER_LIST_NEXT_NBL(pPrev) = NET_BUFFER_LIST_NEXT_NBL(pNext);
				/* stop procesing */
				pPrev = NULL;
				Removed = pNBL;
			}
			else
			{
				pPrev = pNext;
			}
		}
	}
	pContext->SendTail = GetTail(pContext->SendHead);
}

/**********************************************************
Removes all non-waiting NBLs from SendHead list
and returns list of all the removed NBLs
Must be called with SendLock acquired
***********************************************************/
PNET_BUFFER_LIST RemoveAllNonWaitingNBLs(PARANDIS_ADAPTER *pContext)
{
	PNET_BUFFER_LIST pNBL = NULL;
	PNET_BUFFER_LIST p = pContext->SendHead;
	while (p)
	{
		if (!HAS_WAITING_PACKETS(p))
		{
			/* remove from queue and attach to list to return */
			RemoveNBL(pContext, p);
			NET_BUFFER_LIST_NEXT_NBL(p) = pNBL;
			pNBL = p;
			p = pContext->SendHead;
		}
		else
		{
			tNetBufferListEntry *pble = (tNetBufferListEntry *)p->Scratch;
			while (!IsListEmpty(&pble->bufferEntries))
			{
				tNetBufferEntry *pnbe = (tNetBufferEntry *)RemoveHeadList(&pble->bufferEntries);
				pble->nBuffersWaiting++;
				OnNetBufferEntryCompleted(pnbe);
			}
			p = NET_BUFFER_LIST_NEXT_NBL(p);
		}
	}
	return pNBL;
}

/*
static void	PrintMDLChain(PNET_BUFFER netBuffer, PSCATTER_GATHER_LIST pSGList)
{
	ULONG ulOffset = NET_BUFFER_DATA_OFFSET(netBuffer);
	ULONG nToCopy = NET_BUFFER_DATA_LENGTH(netBuffer);
	PMDL  pMDL = NET_BUFFER_FIRST_MDL(netBuffer);
	UINT i;
	DPrintf(0, ("Packet %p, current MDL %p, curMDLOffset %d, nToCopy %d",
		netBuffer, NET_BUFFER_CURRENT_MDL(netBuffer),
		NET_BUFFER_CURRENT_MDL_OFFSET(netBuffer),
		nToCopy));
	while (pMDL && nToCopy)
	{
		ULONG len;
		PVOID addr;
		NdisQueryMdl(pMDL, &addr, &len, NormalPagePriority);
		DPrintf(0, ("MDL %p, offset %d, len %d", pMDL, ulOffset, len));
		if (ulOffset < len)
		{
			len -= ulOffset;
			if (len > nToCopy) len = nToCopy;
			nToCopy -= len;
		}
		else
			ulOffset -= len;
		pMDL = pMDL->Next;
	}
	for (i = 0; i < pSGList->NumberOfElements; ++i)
	{
		PHYSICAL_ADDRESS ph = pSGList->Elements[i].Address;
		DPrintf(0, ("HW buffer[%d]=%d@%08lX:%08lX",
		i, pSGList->Elements[i].Length, ph.HighPart, ph.LowPart));
	}
}
*/
static FORCEINLINE void InitializeTransferParameters(tNetBufferEntry *pnbe, tTxOperationParameters *pParams)
{
	UCHAR protocol = (UCHAR)NET_BUFFER_LIST_INFO(pnbe->nbl, NetBufferListProtocolId);
	tNetBufferListEntry *pble = (tNetBufferListEntry *)pnbe->nbl->Scratch;
	pParams->ReferenceValue = pnbe;
	pParams->packet = pnbe->netBuffer;
	pParams->ulDataSize = NET_BUFFER_DATA_LENGTH(pnbe->netBuffer);
	pParams->offloalMss = pble->mss;
	pParams->flags = pParams->offloalMss ? pcrLSO : 0;
	/*
	NdisQueryNetBufferPhysicalCount(pnbe->netBuffer)
	may give wrong number of fragment, bigger due to current offset
	*/
	pParams->nofSGFragments = pnbe->pSGList ?
		pnbe->pSGList->NumberOfElements : 0;
	//if (pnbe->pSGList) PrintMDLChain(pParams->packet, pnbe->pSGList);
	if (protocol == NDIS_PROTOCOL_ID_TCP_IP)
	{
		pParams->flags |= pcrIsIP;
	}
}


/**********************************************************
Implements NDIS6-specific processing of TX path
Parameters:
	context
	BOOLEAN IsDpc				NDIS wants it
	BOOLEAN bFromInterrupt		FALSE when called during Send operation
***********************************************************/
VOID ParaNdis_ProcessTx(PARANDIS_ADAPTER *pContext, BOOLEAN IsDpc)
{
	PNET_BUFFER_LIST pNBLFailNow = NULL, pNBLReturnNow = NULL;
	ONPAUSECOMPLETEPROC CallbackToCall = NULL;
	NDIS_STATUS status = NDIS_STATUS_FAILURE;

	NdisAcquireSpinLock(&pContext->SendLock);
	ParaNdis_DebugHistory(pContext, hopTxProcess, NULL, 1, pContext->nofFreeHardwareBuffers, pContext->nofFreeTxDescriptors);
	/* try to free something, if we're out of buffers */
	if(IsTimeToReleaseTx(pContext))
	{
		// release some buffers
		ParaNdis_VirtIONetReleaseTransmitBuffers(pContext);
	}
	if (!pContext->bConnected || pContext->SendState != srsEnabled)
	{
		pNBLFailNow = RemoveAllNonWaitingNBLs(pContext);
		if (pContext->SendState != srsEnabled ) status = NDIS_STATUS_PAUSED;
		if (!pContext->bConnected) status = NDIS_STATUS_MEDIA_DISCONNECTED;
		if (pNBLFailNow)
		{
			DPrintf(0, (__FUNCTION__ " Failing send"));
		}
	}
	else if (pContext->SendHead)
	{
		PNET_BUFFER_LIST pCurrent;
		UINT nBuffersSent = 0;
		UINT nBytesSent = 0;
		pCurrent = pContext->SendHead;
		while (pCurrent)
		{
			BOOLEAN bCanSend;
			DPrintf(3, ("[%s] NBL %p", __FUNCTION__, pCurrent));
			/* remove next NBL from the head of the list */
			pContext->SendHead = NET_BUFFER_LIST_NEXT_NBL(pCurrent);
			NET_BUFFER_LIST_NEXT_NBL(pCurrent) = NULL;
			/* can we send it now? */
			//DPrintf(3, (__FUNCTION__ " To send %d buffers(%d b.), max %d", nBuffers, ulMaxSize, ETH_MAX_PACKET_SIZE));
			bCanSend = pContext->nofFreeTxDescriptors != 0;
			if (bCanSend)
			{
				tNetBufferListEntry *pble = (tNetBufferListEntry *)pCurrent->Scratch;
				if (!IsListEmpty(&pble->bufferEntries))
				{
					tCopyPacketResult result;
					tTxOperationParameters Params;
					tNetBufferEntry *pnbe = (tNetBufferEntry *)RemoveHeadList(&pble->bufferEntries);
					InitializeTransferParameters(pnbe, &Params);
					DPrintf(3, ("[%s] Sending pble %p, nbe %p", __FUNCTION__, pble, pnbe));
					result = ParaNdis_DoSubmitPacket(pContext, &Params);
					switch (result.error)
					{
						case cpeInternalError:
						case cpeOK:
						case cpeTooLarge:
							// if this NBL finished?
							pble->nBuffersWaiting++;
							ParaNdis_DebugHistory(pContext, hopSubmittedPacket, pble->nbl, pble->nBuffersWaiting, result.error, Params.flags);
							if (!IsListEmpty(&pble->bufferEntries))
							{
								// no, insert it back to the queue
								NET_BUFFER_LIST_NEXT_NBL(pCurrent) = pContext->SendHead;
								pContext->SendHead = pCurrent;
							}
							else
							{
								// yes, move it to waiting list
								NET_BUFFER_LIST_NEXT_NBL(pCurrent) = pContext->SendWaitingList;
								pContext->SendWaitingList = pCurrent;
								pCurrent = pContext->SendHead;
							}
							if (result.error == cpeOK)
							{
								nBuffersSent++;
								nBytesSent += result.size;
							}

							if (result.error != cpeOK)
							{
								OnNetBufferEntryCompleted(pnbe);
							}
							break;
						case cpeNoBuffer:
							// insert the entry back to the list
							InsertHeadList(&pble->bufferEntries, &pnbe->list);
							// insert the NBL back to the queue
							NET_BUFFER_LIST_NEXT_NBL(pCurrent) = pContext->SendHead;
							pContext->SendHead = pCurrent;
							// break the loop, allow to kick and free some buffers
							pCurrent = NULL;
							break;
					}
				}
				else
				{
					//should not happen, but if any
					NET_BUFFER_LIST_NEXT_NBL(pCurrent) = pContext->SendWaitingList;
					pContext->SendWaitingList = pCurrent;
					pCurrent = pContext->SendHead;
				}
			}
			else
			{
				/* return it to the head of the list */
				NET_BUFFER_LIST_NEXT_NBL(pCurrent) = pContext->SendHead;
				pContext->SendHead = pCurrent;
				/* stop processing, there is nothing to do */
				pCurrent = NULL;
				DPrintf(1, ("[%s] No free TX buffers, waiting...", __FUNCTION__));
			}
		}
		pContext->SendTail = GetTail(pContext->SendHead);
		if (nBuffersSent)
		{
#ifdef PARANDIS_TEST_TX_KICK_ALWAYS
			pContext->NetSendQueue->vq_ops->kick_always(pContext->NetSendQueue);
#else
			pContext->NetSendQueue->vq_ops->kick(pContext->NetSendQueue);
#endif
			DPrintf(2, ("[%s] sent down %d p.(%d b.)", __FUNCTION__, nBuffersSent, nBytesSent));
		}
	}

	/* process waiting list for completion of all the finished NBLs*/
	if (pContext->SendWaitingList)
	{
		PNET_BUFFER_LIST pLookingAt = pContext->SendWaitingList;
		PNET_BUFFER_LIST pPrev = NULL;
		tNetBufferListEntry *pble;
		// traverse the entire the waiting list
		do
		{
			PNET_BUFFER_LIST next = NET_BUFFER_LIST_NEXT_NBL(pLookingAt);
			pble = (tNetBufferListEntry *)pLookingAt->Scratch;
			if (pble->nBuffersDone == pble->nBuffers)
			{
				// the entry is done, move it to completion list
				NET_BUFFER_LIST_NEXT_NBL(pLookingAt) = pNBLReturnNow;
				pNBLReturnNow = pLookingAt;
				// was it at the head of waiting list?
				if (pLookingAt == pContext->SendWaitingList)
				{
					// yes, move the head of waiting list
					pContext->SendWaitingList = next;
				}
				else
				{
					// no, it is already in the middle
					NET_BUFFER_LIST_NEXT_NBL(pPrev) = next;
				}
			}
			else
			{
				// the entry stays in the waiting list, it points on the next entry to check
				pPrev = pLookingAt;
			}
			pLookingAt = next;
		} while (pLookingAt);
	}

	if (IsListEmpty(&pContext->NetSendBuffersInUse) && pContext->SendState == srsPausing)
	{
		CallbackToCall = pContext->SendPauseCompletionProc;
		pContext->SendPauseCompletionProc = NULL;
		pContext->SendState = srsDisabled;
	}
	NdisReleaseSpinLock(&pContext->SendLock);
	if (pNBLFailNow)
	{
		CompleteBufferLists(pContext, pNBLFailNow, status, IsDpc);
	}
	if (pNBLReturnNow)
	{
		CompleteBufferLists(pContext, pNBLReturnNow, NDIS_STATUS_SUCCESS, IsDpc);
	}
	if (CallbackToCall)
	{
		ParaNdis_DebugHistory(pContext, hopInternalSendPause, NULL, 0, 0, 0);
		CallbackToCall(pContext);
	}
}

/**********************************************************
Pauses of restarts TX activity.
Restart is immediate, pause may be delayed until
we return all the NBLs to NDIS

Parameters:
	context
	bPause 1/0 - pause or restart
	ONPAUSECOMPLETEPROC Callback to be called when PAUSE finished
Return value:
	SUCCESS if finished synchronously
	PENDING if not, then callback will be called later
***********************************************************/
NDIS_STATUS ParaNdis6_SendPauseRestart(
	PARANDIS_ADAPTER *pContext,
	BOOLEAN bPause,
	ONPAUSECOMPLETEPROC Callback
)
{
	PNET_BUFFER_LIST pNBL = NULL;
	NDIS_STATUS status = NDIS_STATUS_SUCCESS;
	DEBUG_ENTRY(4);
	NdisAcquireSpinLock(&pContext->SendLock);
	if (bPause)
	{
		ParaNdis_DebugHistory(pContext, hopInternalSendPause, NULL, 1, 0, 0);
		if (pContext->SendState == srsEnabled)
		{
			if (IsListEmpty(&pContext->NetSendBuffersInUse) && !pContext->SendWaitingList)
			{
				pNBL = pContext->SendHead;
				pContext->SendHead = pContext->SendTail = NULL;
				pContext->SendState = srsDisabled;
			}
			else
			{
				pContext->SendState = srsPausing;
				pContext->SendPauseCompletionProc = Callback;
				status = NDIS_STATUS_PENDING;
				/* remove from send queue all the NBL whose transfer did not start */
				pNBL = RemoveAllNonWaitingNBLs(pContext);
			}
		}
		if (status == NDIS_STATUS_SUCCESS)
		{
			ParaNdis_DebugHistory(pContext, hopInternalSendPause, NULL, 0, 0, 0);
		}
	}
	else
	{
		pContext->SendState = srsEnabled;
		ParaNdis_DebugHistory(pContext, hopInternalSendResume, NULL, 0, 0, 0);
	}
	NdisReleaseSpinLock(&pContext->SendLock);
	if (pNBL) CompleteBufferLists(pContext, pNBL, NDIS_STATUS_PAUSED, FALSE);
	return status;
}

/**********************************************************
Required procedure of NDIS
NDIS wants to cancel sending of each list which has specified CancelID
Can be tested only under NDIS Test
***********************************************************/
VOID ParaNdis6_CancelSendNetBufferLists(
	NDIS_HANDLE  miniportAdapterContext,
	PVOID pCancelId)
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)miniportAdapterContext;
	PNET_BUFFER_LIST pNBLCancel = NULL, pNBL;
	UINT nCancelled = 0;
	DEBUG_ENTRY(0);
	NdisAcquireSpinLock(&pContext->SendLock);
	pNBL = pContext->SendHead;
	while (pNBL)
	{
		// save next
		PNET_BUFFER_LIST Next = NET_BUFFER_LIST_NEXT_NBL(pNBL);
		if (NDIS_GET_NET_BUFFER_LIST_CANCEL_ID(pNBL) == pCancelId && !HAS_WAITING_PACKETS(pNBL))
		{
			// remove from queue and waiting list
			RemoveNBL(pContext, pNBL);
			/* insert it to the list of cancellation */
			NET_BUFFER_LIST_NEXT_NBL(pNBL) = pNBLCancel;
			pNBLCancel = pNBL;
			nCancelled++;
			/* restart processing */
			pNBL = pContext->SendHead;
		}
		else
		{
			// goto next
			pNBL = Next;
		}
	}
	pContext->SendTail = GetTail(pContext->SendHead);
	NdisReleaseSpinLock(&pContext->SendLock);
	if (pNBLCancel)
	{
		CompleteBufferLists(pContext, pNBLCancel, NDIS_STATUS_SEND_ABORTED, FALSE);
	}
	DEBUG_EXIT_STATUS(0, nCancelled);
}

#define VISTA_RECOVERY_CANCEL_TIMER						1
#define VISTA_RECOVERY_RUN_DPC							2
#define VISTA_RECOVERY_INFO_ONLY_SECOND_READ			4


static UCHAR MiniportSyncRecoveryProcedure(PVOID  SynchronizeContext)
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)SynchronizeContext;
	BOOLEAN b;
	UCHAR val = 0;
	if (pContext->ulIrqReceived)
	{
		val = VISTA_RECOVERY_CANCEL_TIMER;
	}
	else
	{
		b = ParaNdis_OnInterrupt(pContext, &b, isAny);
		if (b)
		{
			// we read the interrupt, in any case run the DRC
			val = VISTA_RECOVERY_RUN_DPC;
			b = !VirtIODeviceISR(&pContext->IODevice);
			// if we read it again, it does not mean anything
			if (b) val |= VISTA_RECOVERY_INFO_ONLY_SECOND_READ;
		}
	}
	return val;
}


VOID ParaNdis6_OnInterruptRecoveryTimer(PARANDIS_ADAPTER *pContext)
{
	UCHAR val;
	val = NdisMSynchronizeWithInterruptEx(
		pContext->InterruptHandle,
		0,
		MiniportSyncRecoveryProcedure,
		pContext);
	if (val & VISTA_RECOVERY_RUN_DPC)
	{
		InterlockedOr(&pContext->InterruptStatus, isAny);
		ParaNdis_DPCWorkBody(pContext);
	}
	if (~val & VISTA_RECOVERY_CANCEL_TIMER)
		ParaNdis_SetTimer(pContext->InterruptRecoveryTimer, 15);
	else
	{
		DPrintf(0, ("[%s] Cancelled", __FUNCTION__));
	}
	DEBUG_EXIT_STATUS(5, val);
}

#endif // NDIS60_MINIPORT
