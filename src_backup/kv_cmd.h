//////////////////////////////////////////////////////////////////////////////////
// kv_cmd.h for Cosmos+ OpenSSD KV-SSD (CSE4116 Project #3)
//
// Key-Value command handler
//   - in-DRAM open-addressing hash index: 4B key -> value slot
//   - value store: one 4KB logical block (slot) per key, overwritten in place
//     on top of the FTL logical address space (FTL handles out-of-place NAND
//     writes and garbage collection underneath)
//////////////////////////////////////////////////////////////////////////////////

#ifndef KV_CMD_H_
#define KV_CMD_H_

#include <stdint.h>
#include "nvme/nvme.h"

//-----------------------------------------------------------------------------
// KV index memory layout
//
// The hash table is statically placed in the reserved DRAM region behind the
// FTL management area (FTL_MANAGEMENT_END_ADDR ~= 0x1A060000 < 0x20000000,
// checked at runtime in InitKV). main.c maps 0x18000000-0x3FFFFFFF as
// cached & buffered memory; only the CPU touches the index, so this is safe.
//-----------------------------------------------------------------------------
#define KV_HASH_TABLE_ADDR			0x20000000
#define KV_HASH_BUCKET_BITS			23
#define KV_HASH_BUCKET_COUNT		(1 << KV_HASH_BUCKET_BITS)			// 8,388,608 buckets
#define KV_HASH_BUCKET_MASK			(KV_HASH_BUCKET_COUNT - 1)
#define KV_HASH_TABLE_END_ADDR		(KV_HASH_TABLE_ADDR + KV_HASH_BUCKET_COUNT * sizeof(KV_HASH_ENTRY))

#define KV_SLOT_NONE				0xffffffff	// empty bucket marker
#define KV_SLOT_FAIL				0xffffffff

// value size is fixed to one NVMe block (4KB) by the host benchmark protocol
#define KV_VALUE_BLOCKS				1

//-----------------------------------------------------------------------------
// NVMe completion status words (NVME_COMPLETION.statusFieldWord layout:
// bit0 reserved(phase), bits[8:1] SC, bits[11:9] SCT)
// The host-visible status (ioctl return) is statusFieldWord >> 1.
//-----------------------------------------------------------------------------
#define KV_CPL_SUCCESS				0x0
#define KV_CPL_NO_SUCH_KEY			((0xC1 << 1) | (SCT_VENDOR_SPECIFIC << 9))	// host sees 0x7C1 (ENOSUCHKEY)
#define KV_CPL_INTERNAL_ERROR		((0xC2 << 1) | (SCT_VENDOR_SPECIFIC << 9))	// host sees 0x7C2

typedef struct _KV_HASH_ENTRY {
	unsigned int key;
	unsigned int slot;		// logical 4KB block address of the value, KV_SLOT_NONE if empty
} KV_HASH_ENTRY, *P_KV_HASH_ENTRY;

typedef struct _KV_HASH_TABLE {
	KV_HASH_ENTRY bucket[KV_HASH_BUCKET_COUNT];
} KV_HASH_TABLE, *P_KV_HASH_TABLE;

void InitKV();
void kv_put(unsigned int cmdSlotTag, unsigned int key, unsigned int valueSize, unsigned int nlb);
void kv_get(unsigned int cmdSlotTag, unsigned int key, unsigned int valueSize, unsigned int nlb);

extern P_KV_HASH_TABLE kvHashTablePtr;
extern unsigned int kvNextFreeSlot;
extern unsigned int kvStoredKeyCount;

#endif /* KV_CMD_H_ */
