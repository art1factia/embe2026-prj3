//////////////////////////////////////////////////////////////////////////////////
// kv_cmd.c for Cosmos+ OpenSSD KV-SSD (CSE4116 Project #3)
//
// Key-Value command handler
//
// Design summary
//   - Index: open-addressing (linear probing) hash table in DRAM at
//     KV_HASH_TABLE_ADDR, mapping a 4-byte key to a value slot.
//   - Value store: each key owns one logical 4KB block ("slot") in the FTL
//     logical address space, allocated on the first PUT from a monotonically
//     increasing counter. A repeated PUT overwrites the same slot in place;
//     wear leveling / out-of-place NAND writes / garbage collection are
//     handled by the underlying GreedyFTL exactly as for block I/O.
//   - PUT reuses the verified block-write path (ReqTransNvmeToSlice):
//     RxDMA into the data buffer, read-modify-write for the partial 16KB
//     slice, lazy NAND flush on eviction, hardware auto-completion.
//   - GET is handled synchronously: drain the request pipeline, locate the
//     slice in the data buffer (or read it from NAND into the buffer), then
//     TX-DMA the 4KB value and complete manually with CQE DW0 = value length.
//////////////////////////////////////////////////////////////////////////////////

#include "xil_printf.h"
#include "nvme/debug.h"
#include "memory_map.h"
#include "nvme/host_lld.h"
#include "kv_cmd.h"

P_KV_HASH_TABLE kvHashTablePtr;
unsigned int kvNextFreeSlot;
unsigned int kvStoredKeyCount;

// limit occupancy so linear probing always terminates well before the table fills
#define KV_MAX_STORED_KEYS		(KV_HASH_BUCKET_COUNT / 2)

static unsigned int KvHash(unsigned int key)
{
	return (key * 2654435761u) & KV_HASH_BUCKET_MASK;
}

void InitKV()
{
	unsigned int bucketNo;

	ASSERT(FTL_MANAGEMENT_END_ADDR < KV_HASH_TABLE_ADDR);
	ASSERT(KV_HASH_TABLE_END_ADDR <= DRAM_END_ADDR);

	kvHashTablePtr = (P_KV_HASH_TABLE)KV_HASH_TABLE_ADDR;

	// mark every bucket empty; KV_SLOT_NONE (not 0) is the empty marker,
	// because key 0 / slot 0 are both valid
	for (bucketNo = 0; bucketNo < KV_HASH_BUCKET_COUNT; bucketNo++)
	{
		kvHashTablePtr->bucket[bucketNo].key = 0xffffffff;
		kvHashTablePtr->bucket[bucketNo].slot = KV_SLOT_NONE;
	}

	kvNextFreeSlot = 0;
	kvStoredKeyCount = 0;

	xil_printf("[ KV index reset: %d buckets (%d MB) at 0x%X ]\r\n",
			KV_HASH_BUCKET_COUNT, (unsigned int)(sizeof(KV_HASH_TABLE) >> 20), KV_HASH_TABLE_ADDR);
}

// returns the slot of `key`, or KV_SLOT_FAIL if the key does not exist
static unsigned int KvLookup(unsigned int key)
{
	unsigned int bucketNo = KvHash(key);

	while (kvHashTablePtr->bucket[bucketNo].slot != KV_SLOT_NONE)
	{
		if (kvHashTablePtr->bucket[bucketNo].key == key)
			return kvHashTablePtr->bucket[bucketNo].slot;

		bucketNo = (bucketNo + 1) & KV_HASH_BUCKET_MASK;
	}

	return KV_SLOT_FAIL;
}

// returns the slot of `key`, allocating a new one on first insertion;
// KV_SLOT_FAIL if the index or the logical value space is exhausted
static unsigned int KvLookupOrInsert(unsigned int key)
{
	unsigned int bucketNo = KvHash(key);

	while (kvHashTablePtr->bucket[bucketNo].slot != KV_SLOT_NONE)
	{
		if (kvHashTablePtr->bucket[bucketNo].key == key)
			return kvHashTablePtr->bucket[bucketNo].slot;

		bucketNo = (bucketNo + 1) & KV_HASH_BUCKET_MASK;
	}

	// new key: check capacity before touching the table so a rejected PUT
	// leaves no stale index entry behind
	if (kvStoredKeyCount >= KV_MAX_STORED_KEYS)
		return KV_SLOT_FAIL;
	if (kvNextFreeSlot + KV_VALUE_BLOCKS > storageCapacity_L)
		return KV_SLOT_FAIL;

	kvHashTablePtr->bucket[bucketNo].key = key;
	kvHashTablePtr->bucket[bucketNo].slot = kvNextFreeSlot;
	kvNextFreeSlot += KV_VALUE_BLOCKS;
	kvStoredKeyCount++;

	return kvHashTablePtr->bucket[bucketNo].slot;
}

// data buffer hash walk for `logicalSliceAddr` (read-only variant of
// CheckDataBufHit): no LRU reordering, no request pool interaction.
// Safe here because kv_get completes synchronously, so no other command can
// allocate or evict buffer entries while we hold the returned entry.
static unsigned int KvFindDataBufEntry(unsigned int logicalSliceAddr)
{
	unsigned int bufEntry;

	bufEntry = dataBufHashTablePtr->dataBufHash[FindDataBufHashTableEntry(logicalSliceAddr)].headEntry;

	while (bufEntry != DATA_BUF_NONE)
	{
		if (dataBufMapPtr->dataBuf[bufEntry].logicalSliceAddr == logicalSliceAddr)
			return bufEntry;

		bufEntry = dataBufMapPtr->dataBuf[bufEntry].hashNextEntry;
	}

	return DATA_BUF_FAIL;
}

// read the slice holding `logicalSliceAddr` from NAND into a data buffer
// entry (mirrors the buffer-miss path of ReqTransSliceToLowLevel) and wait
// for completion; returns the buffer entry, or DATA_BUF_FAIL if the slice
// has never been written to NAND
static unsigned int KvReadSliceFromNand(unsigned int cmdSlotTag, unsigned int logicalSliceAddr)
{
	unsigned int reqSlotTag, bufEntry, virtualSliceAddr;

	virtualSliceAddr = AddrTransRead(logicalSliceAddr);
	if (virtualSliceAddr == VSA_FAIL)
		return DATA_BUF_FAIL;

	reqSlotTag = GetFromFreeReqQ();
	bufEntry = AllocateDataBuf();

	// EvictDataBufEntry reads dataBufInfo.entry/nvmeCmdSlotTag from this
	// request and flushes the entry's previous dirty content keyed by the
	// OLD logicalSliceAddr, so the entry metadata is updated only afterwards
	reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
	reqPoolPtr->reqPool[reqSlotTag].nvmeCmdSlotTag = cmdSlotTag;
	reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = bufEntry;

	EvictDataBufEntry(reqSlotTag);

	dataBufMapPtr->dataBuf[bufEntry].logicalSliceAddr = logicalSliceAddr;
	PutToDataBufHashList(bufEntry);

	UpdateDataBufEntryInfoBlockingReq(bufEntry, reqSlotTag);
	reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;

	SelectLowLevelReqQ(reqSlotTag);
	SyncAllLowLevelReqDone();

	return bufEntry;
}

void kv_put(unsigned int cmdSlotTag, unsigned int key, unsigned int valueSize, unsigned int nlb)
{
	unsigned int slot;

	// the host benchmark protocol fixes the value size to one 4KB block;
	// reject anything else instead of corrupting neighbor slots
	if (nlb + 1 != KV_VALUE_BLOCKS)
	{
		xil_printf("KV-PUT: unsupported value size (nlb=%d, size=%d)\r\n", nlb, valueSize);
		set_auto_nvme_cpl(cmdSlotTag, 0, KV_CPL_INTERNAL_ERROR);
		return;
	}

	slot = KvLookupOrInsert(key);
	if (slot == KV_SLOT_FAIL)
	{
		xil_printf("KV-PUT: index/capacity full (keys=%d)\r\n", kvStoredKeyCount);
		set_auto_nvme_cpl(cmdSlotTag, 0, KV_CPL_INTERNAL_ERROR);
		return;
	}

	// reuse the verified block-write path: RxDMA into the data buffer
	// (read-modify-write of the surrounding 16KB slice is handled there),
	// hardware auto-completion reports success with CQE DW0 = 0.
	// If the hardware turned out to post a nonzero DW0, the fallback is a
	// manual synchronous completion like the kv_get path (RxDMA with
	// auto-completion off + set_auto_nvme_cpl(cmdSlotTag, 0, KV_CPL_SUCCESS)).
	ReqTransNvmeToSlice(cmdSlotTag, slot, KV_VALUE_BLOCKS - 1, IO_NVM_WRITE);
}

void kv_get(unsigned int cmdSlotTag, unsigned int key, unsigned int valueSize, unsigned int nlb)
{
	unsigned int slot, logicalSliceAddr, bufEntry, devAddr;

	slot = KvLookup(key);
	if (slot == KV_SLOT_FAIL)
	{
		// completes the command and releases the slot, host sees ENOSUCHKEY
		set_auto_nvme_cpl(cmdSlotTag, 0, KV_CPL_NO_SUCH_KEY);
		return;
	}

	// settle all in-flight RxDMA/NAND work so the data buffer and the
	// mapping table reflect every previously completed PUT
	SyncAllLowLevelReqDone();

	logicalSliceAddr = slot / NVME_BLOCKS_PER_SLICE;

	bufEntry = KvFindDataBufEntry(logicalSliceAddr);
	if (bufEntry == DATA_BUF_FAIL)
		bufEntry = KvReadSliceFromNand(cmdSlotTag, logicalSliceAddr);

	if (bufEntry == DATA_BUF_FAIL)
	{
		// index says the key exists but no data is buffered or mapped;
		// must not happen - report an error instead of transferring garbage
		xil_printf("KV-GET: missing value data (key=0x%X, slot=%d)\r\n", key, slot);
		set_auto_nvme_cpl(cmdSlotTag, 0, KV_CPL_INTERNAL_ERROR);
		return;
	}

	devAddr = DATA_BUFFER_BASE_ADDR + bufEntry * BYTES_PER_DATA_REGION_OF_SLICE
			+ (slot % NVME_BLOCKS_PER_SLICE) * BYTES_PER_NVME_BLOCK;

	// transfer the 4KB value to the head of the host buffer, then complete
	// manually so CQE DW0 carries the value length (the pipeline was drained
	// above, so this is the only outstanding TX DMA the global wait sees)
	set_auto_tx_dma(cmdSlotTag, 0, devAddr, NVME_COMMAND_AUTO_COMPLETION_OFF);
	check_auto_tx_dma_done();

	set_auto_nvme_cpl(cmdSlotTag, KV_VALUE_BLOCKS * BYTES_PER_NVME_BLOCK, KV_CPL_SUCCESS);
}
