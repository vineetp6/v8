// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/spaces.h"

#include <algorithm>
#include <cinttypes>
#include <utility>

#include "src/base/bits.h"
#include "src/base/bounded-page-allocator.h"
#include "src/base/macros.h"
#include "src/base/sanitizer/msan.h"
#include "src/common/globals.h"
#include "src/heap/base/active-system-pages.h"
#include "src/heap/concurrent-allocator.h"
#include "src/heap/concurrent-marking.h"
#include "src/heap/heap.h"
#include "src/heap/incremental-marking-inl.h"
#include "src/heap/large-spaces.h"
#include "src/heap/main-allocator-inl.h"
#include "src/heap/mark-compact.h"
#include "src/heap/memory-chunk-layout.h"
#include "src/heap/memory-chunk.h"
#include "src/heap/read-only-heap.h"
#include "src/heap/remembered-set.h"
#include "src/heap/slot-set.h"
#include "src/init/v8.h"
#include "src/logging/counters.h"
#include "src/objects/free-space-inl.h"
#include "src/objects/heap-object.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/objects-inl.h"
#include "src/snapshot/snapshot.h"
#include "src/utils/ostreams.h"

namespace v8 {
namespace internal {

SpaceWithLinearArea::SpaceWithLinearArea(
    Heap* heap, AllocationSpace id, std::unique_ptr<FreeList> free_list,
    AllocationCounter& allocation_counter,
    LinearAllocationArea& allocation_info,
    LinearAreaOriginalData& linear_area_original_data)
    : Space(heap, id, std::move(free_list)),
      allocator_(heap, this, allocation_counter, allocation_info,
                 linear_area_original_data) {}

Address SpaceWithLinearArea::ComputeLimit(Address start, Address end,
                                          size_t min_size) const {
  DCHECK_GE(end - start, min_size);

  // During GCs we always use the full LAB.
  if (heap()->IsInGC()) return end;

  if (!heap()->IsInlineAllocationEnabled()) {
    // LABs are disabled, so we fit the requested area exactly.
    return start + min_size;
  }

  // When LABs are enabled, pick the largest possible LAB size by default.
  size_t step_size = end - start;

  if (SupportsAllocationObserver() && heap()->IsAllocationObserverActive()) {
    // Ensure there are no unaccounted allocations.
    DCHECK_EQ(allocator_.allocation_info().start(),
              allocator_.allocation_info().top());

    size_t step = allocator_.allocation_counter().NextBytes();
    DCHECK_NE(step, 0);
    // Generated code may allocate inline from the linear allocation area. To
    // make sure we can observe these allocations, we use a lower limit.
    size_t rounded_step = static_cast<size_t>(
        RoundSizeDownToObjectAlignment(static_cast<int>(step - 1)));
    step_size = std::min(step_size, rounded_step);
  }

  if (v8_flags.stress_marking) {
    step_size = std::min(step_size, static_cast<size_t>(64));
  }

  DCHECK_LE(start + step_size, end);
  return start + std::max(step_size, min_size);
}

LinearAllocationArea LocalAllocationBuffer::CloseAndMakeIterable() {
  if (IsValid()) {
    MakeIterable();
    const LinearAllocationArea old_info = allocation_info_;
    allocation_info_ = LinearAllocationArea(kNullAddress, kNullAddress);
    return old_info;
  }
  return LinearAllocationArea(kNullAddress, kNullAddress);
}

void LocalAllocationBuffer::MakeIterable() {
  if (IsValid()) {
    heap_->CreateFillerObjectAtBackground(
        allocation_info_.top(),
        static_cast<int>(allocation_info_.limit() - allocation_info_.top()));
  }
}

LocalAllocationBuffer::LocalAllocationBuffer(
    Heap* heap, LinearAllocationArea allocation_info) V8_NOEXCEPT
    : heap_(heap),
      allocation_info_(allocation_info) {}

LocalAllocationBuffer::LocalAllocationBuffer(LocalAllocationBuffer&& other)
    V8_NOEXCEPT {
  *this = std::move(other);
}

LocalAllocationBuffer& LocalAllocationBuffer::operator=(
    LocalAllocationBuffer&& other) V8_NOEXCEPT {
  heap_ = other.heap_;
  allocation_info_ = other.allocation_info_;

  other.allocation_info_.Reset(kNullAddress, kNullAddress);
  return *this;
}

void SpaceWithLinearArea::AddAllocationObserver(AllocationObserver* observer) {
  allocator_.AddAllocationObserver(observer);
}

void SpaceWithLinearArea::RemoveAllocationObserver(
    AllocationObserver* observer) {
  allocator_.RemoveAllocationObserver(observer);
}

void SpaceWithLinearArea::PauseAllocationObservers() {
  allocator_.PauseAllocationObservers();
}

void SpaceWithLinearArea::ResumeAllocationObservers() {
  allocator_.ResumeAllocationObservers();
}

void SpaceWithLinearArea::AdvanceAllocationObservers() {
  allocator_.AdvanceAllocationObservers();
}

void SpaceWithLinearArea::MarkLabStartInitialized() {
  allocator_.MarkLabStartInitialized();
}

void SpaceWithLinearArea::InvokeAllocationObservers(
    Address soon_object, size_t size_in_bytes, size_t aligned_size_in_bytes,
    size_t allocation_size) {
  allocator_.InvokeAllocationObservers(soon_object, size_in_bytes,
                                       aligned_size_in_bytes, allocation_size);
}

AllocationResult SpaceWithLinearArea::AllocateRawForceAlignmentForTesting(
    int size_in_bytes, AllocationAlignment alignment, AllocationOrigin origin) {
  return allocator_.AllocateRawForceAlignmentForTesting(size_in_bytes,
                                                        alignment, origin);
}

#if DEBUG
void SpaceWithLinearArea::VerifyTop() const {
  // Ensure validity of LAB: start <= top <= limit
  DCHECK_LE(allocator_.allocation_info().start(),
            allocator_.allocation_info().top());
  DCHECK_LE(allocator_.allocation_info().top(),
            allocator_.allocation_info().limit());
}
#endif  // DEBUG

SpaceIterator::SpaceIterator(Heap* heap)
    : heap_(heap), current_space_(FIRST_MUTABLE_SPACE) {}

SpaceIterator::~SpaceIterator() = default;

bool SpaceIterator::HasNext() {
  while (current_space_ <= LAST_MUTABLE_SPACE) {
    Space* space = heap_->space(current_space_);
    if (space) return true;
    ++current_space_;
  }

  // No more spaces left.
  return false;
}

Space* SpaceIterator::Next() {
  DCHECK_LE(current_space_, LAST_MUTABLE_SPACE);
  Space* space = heap_->space(current_space_++);
  DCHECK_NOT_NULL(space);
  return space;
}

}  // namespace internal
}  // namespace v8
