#include "duckdb/common/types/column/column_data_allocator.hpp"

#include "duckdb/common/radix_partitioning.hpp"
#include "duckdb/common/types/column/column_data_collection_segment.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

ColumnDataAllocator::ColumnDataAllocator(Allocator &allocator) : type(ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR) {
	alloc.allocator = &allocator;
}

ColumnDataAllocator::ColumnDataAllocator(BufferManager &buffer_manager)
    : type(ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR) {
	alloc.buffer_manager = &buffer_manager;
}

ColumnDataAllocator::ColumnDataAllocator(ClientContext &context, ColumnDataAllocatorType allocator_type)
    : type(allocator_type) {
	switch (type) {
	case ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR:
	case ColumnDataAllocatorType::HYBRID:
		alloc.buffer_manager = &BufferManager::GetBufferManager(context);
		break;
	case ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR:
		alloc.allocator = &Allocator::Get(context);
		break;
	default:
		throw InternalException("Unrecognized column data allocator type");
	}
}

ColumnDataAllocator::ColumnDataAllocator(ColumnDataAllocator &other) {
	type = other.GetType();
	switch (type) {
	case ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR:
	case ColumnDataAllocatorType::HYBRID:
		alloc.allocator = other.alloc.allocator;
		break;
	case ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR:
		alloc.buffer_manager = other.alloc.buffer_manager;
		break;
	default:
		throw InternalException("Unrecognized column data allocator type");
	}
}

ColumnDataAllocator::~ColumnDataAllocator() {
	if (type == ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR) {
		return;
	}
	for (auto &block : blocks) {
		block.handle->SetDestroyBufferUpon(DestroyBufferUpon::UNPIN);
	}
	blocks.clear();
}

BufferHandle ColumnDataAllocator::Pin(uint32_t block_id) {
	D_ASSERT(type == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR || type == ColumnDataAllocatorType::HYBRID);
	shared_ptr<BlockHandle> handle;
	if (shared) {
		// we only need to grab the lock when accessing the vector, because vector access is not thread-safe:
		// the vector can be resized by another thread while we try to access it
		lock_guard<mutex> guard(lock);
		handle = blocks[block_id].handle;
	} else {
		handle = blocks[block_id].handle;
	}
	return alloc.buffer_manager->Pin(handle);
}

BufferHandle ColumnDataAllocator::AllocateBlock(idx_t size) {
	D_ASSERT(type == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR || type == ColumnDataAllocatorType::HYBRID);
	auto max_size = MaxValue<idx_t>(size, GetBufferManager().GetBlockSize());
	BlockMetaData data;
	data.size = 0;
	data.capacity = NumericCast<uint32_t>(max_size);
	auto pin = alloc.buffer_manager->Allocate(MemoryTag::COLUMN_DATA, max_size, false);
	data.handle = pin.GetBlockHandle();
	blocks.push_back(std::move(data));
	if (partition_index.IsValid()) { // Set the eviction queue index logarithmically using RadixBits
		blocks.back().handle->SetEvictionQueueIndex(RadixPartitioning::RadixBits(partition_index.GetIndex()));
	}
	allocated_size += max_size;
	return pin;
}

void ColumnDataAllocator::AllocateEmptyBlock(idx_t size) {
	auto allocation_amount = MaxValue<idx_t>(NextPowerOfTwo(size), 4096);
	if (!blocks.empty()) {
		idx_t last_capacity = blocks.back().capacity;
		auto next_capacity = MinValue<idx_t>(last_capacity * 2, last_capacity + Storage::DEFAULT_BLOCK_SIZE);
		allocation_amount = MaxValue<idx_t>(next_capacity, allocation_amount);
	}
	D_ASSERT(type == ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR);
	BlockMetaData data;
	data.size = 0;
	data.capacity = NumericCast<uint32_t>(allocation_amount);
	data.handle = nullptr;
	blocks.push_back(std::move(data));
	allocated_size += allocation_amount;
}

void ColumnDataAllocator::AssignPointer(uint32_t &block_id, uint32_t &offset, data_ptr_t pointer) {
	auto pointer_value = uintptr_t(pointer);
	if (sizeof(uintptr_t) == sizeof(uint32_t)) {
		block_id = uint32_t(pointer_value);
	} else if (sizeof(uintptr_t) == sizeof(uint64_t)) {
		block_id = uint32_t(pointer_value & 0xFFFFFFFF);
		offset = uint32_t(pointer_value >> 32);
	} else {
		throw InternalException("ColumnDataCollection: Architecture not supported!?");
	}
}

void ColumnDataAllocator::AllocateBuffer(idx_t size, uint32_t &block_id, uint32_t &offset,
                                         ChunkManagementState *chunk_state) {
	D_ASSERT(allocated_data.empty());
	if (blocks.empty() || blocks.back().Capacity() < size) {
		auto pinned_block = AllocateBlock(size);
		if (chunk_state) {
			D_ASSERT(!blocks.empty());
			auto new_block_id = blocks.size() - 1;
			chunk_state->handles[new_block_id] = std::move(pinned_block);
		}
	}
	auto &block = blocks.back();
	D_ASSERT(size <= block.capacity - block.size);
	block_id = NumericCast<uint32_t>(blocks.size() - 1);
	if (chunk_state && chunk_state->handles.find(block_id) == chunk_state->handles.end()) {
		// not guaranteed to be pinned already by this thread (if shared allocator)
		chunk_state->handles[block_id] = alloc.buffer_manager->Pin(blocks[block_id].handle);
	}
	offset = block.size;
	block.size += size;
}

void ColumnDataAllocator::AllocateMemory(idx_t size, uint32_t &block_id, uint32_t &offset,
                                         ChunkManagementState *chunk_state) {
	D_ASSERT(blocks.size() == allocated_data.size());
	if (blocks.empty() || blocks.back().Capacity() < size) {
		AllocateEmptyBlock(size);
		auto &last_block = blocks.back();
		auto allocated = alloc.allocator->Allocate(last_block.capacity);
		allocated_data.push_back(std::move(allocated));
	}
	auto &block = blocks.back();
	D_ASSERT(size <= block.capacity - block.size);
	AssignPointer(block_id, offset, allocated_data.back().get() + block.size);
	block.size += size;
}

void ColumnDataAllocator::AllocateData(idx_t size, uint32_t &block_id, uint32_t &offset,
                                       ChunkManagementState *chunk_state) {
	switch (type) {
	case ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR:
	case ColumnDataAllocatorType::HYBRID:
		if (shared) {
			lock_guard<mutex> guard(lock);
			AllocateBuffer(size, block_id, offset, chunk_state);
		} else {
			AllocateBuffer(size, block_id, offset, chunk_state);
		}
		break;
	case ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR:
		D_ASSERT(!shared);
		AllocateMemory(size, block_id, offset, chunk_state);
		break;
	default:
		throw InternalException("Unrecognized allocator type");
	}
}

void ColumnDataAllocator::Initialize(ColumnDataAllocator &other) {
	D_ASSERT(other.HasBlocks());
	blocks.push_back(other.blocks.back());
}

data_ptr_t ColumnDataAllocator::GetDataPointer(ChunkManagementState &state, uint32_t block_id, uint32_t offset) {
	if (type == ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR) {
		// in-memory allocator: construct pointer from block_id and offset
		if (sizeof(uintptr_t) == sizeof(uint32_t)) {
			uintptr_t pointer_value = uintptr_t(block_id);
			return (data_ptr_t)pointer_value; // NOLINT - convert from pointer value back to pointer
		} else if (sizeof(uintptr_t) == sizeof(uint64_t)) {
			uintptr_t pointer_value = (uintptr_t(offset) << 32) | uintptr_t(block_id);
			return (data_ptr_t)pointer_value; // NOLINT - convert from pointer value back to pointer
		} else {
			throw InternalException("ColumnDataCollection: Architecture not supported!?");
		}
	}
	D_ASSERT(state.handles.find(block_id) != state.handles.end());
	return state.handles[block_id].Ptr() + offset;
}

void ColumnDataAllocator::UnswizzlePointers(ChunkManagementState &state, Vector &result,
                                            SwizzleMetaData &swizzle_segment, const VectorMetaData &string_heap_segment,
                                            const idx_t &v_offset, const bool &copied) {
	D_ASSERT(result.GetType().InternalType() == PhysicalType::VARCHAR);
	lock_guard<mutex> guard(lock);
	const auto old_base_ptr = char_ptr_cast(swizzle_segment.ptr);
	const auto new_base_ptr =
	    char_ptr_cast(GetDataPointer(state, string_heap_segment.block_id, string_heap_segment.offset));
	if (old_base_ptr == new_base_ptr) {
		return; // pointers are still valid
	}

	const auto &validity = FlatVector::Validity(result);
	const auto strings = FlatVector::GetData<string_t>(result);

	// recompute pointers
	const auto start = NumericCast<idx_t>(v_offset + swizzle_segment.offset);
	const auto end = start + NumericCast<idx_t>(swizzle_segment.count);
	for (idx_t i = start; i < end; i++) {
		auto &str = strings[i];
		if (!validity.RowIsValid(i) || str.IsInlined()) {
			continue;
		}
		const auto str_offset = str.GetPointer() - old_base_ptr;
		D_ASSERT(str_offset >= 0);
		str.SetPointer(new_base_ptr + str_offset);
#ifdef D_ASSERT_IS_ENABLED
		if (result.GetType() == LogicalType::VARCHAR) {
			str.Verify();
		}
#endif
	}

	if (!copied) {
		// if the data was not copied, we modified data on the blocks. store the new base ptr
		swizzle_segment.ptr = data_ptr_cast(new_base_ptr);
	}
}

void ColumnDataAllocator::SetDestroyBufferUponUnpin(uint32_t block_id) {
	blocks[block_id].handle->SetDestroyBufferUpon(DestroyBufferUpon::UNPIN);
}

Allocator &ColumnDataAllocator::GetAllocator() {
	if (type == ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR) {
		return *alloc.allocator;
	}
	return alloc.buffer_manager->GetBufferAllocator();
}

BufferManager &ColumnDataAllocator::GetBufferManager() {
	if (type == ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR) {
		throw InternalException("cannot obtain the buffer manager for in memory allocations");
	}
	return *alloc.buffer_manager;
}

void ColumnDataAllocator::InitializeChunkState(ChunkManagementState &state, ChunkMetaData &chunk) {
	if (type != ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR && type != ColumnDataAllocatorType::HYBRID) {
		// nothing to pin
		return;
	}
	// release any handles that are no longer required
	bool found_handle;
	do {
		found_handle = false;
		for (auto it = state.handles.begin(); it != state.handles.end(); it++) {
			if (chunk.block_ids.find(NumericCast<uint32_t>(it->first)) != chunk.block_ids.end()) {
				// still required: do not release
				continue;
			}
			state.handles.erase(it);
			found_handle = true;
			break;
		}
	} while (found_handle);

	// grab any handles that are now required
	for (auto &block_id : chunk.block_ids) {
		if (state.handles.find(block_id) != state.handles.end()) {
			// already pinned: don't need to do anything
			continue;
		}
		state.handles[block_id] = Pin(block_id);
	}
}

uint32_t BlockMetaData::Capacity() {
	D_ASSERT(size <= capacity);
	return capacity - size;
}

} // namespace duckdb
