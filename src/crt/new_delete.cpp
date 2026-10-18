/**
 * @file new_delete.cpp
 * @brief C++20 global operator new / operator delete bindings to KernelHeap.
 */

#include "../memory/heap_allocator.hpp"
#include <stddef.h>

void* operator new(size_t size) {
    return cortez::memory::KernelHeap::instance().allocate(size);
}

void* operator new[](size_t size) {
    return cortez::memory::KernelHeap::instance().allocate(size);
}

void operator delete(void* ptr) noexcept {
    cortez::memory::KernelHeap::instance().deallocate(ptr);
}

void operator delete[](void* ptr) noexcept {
    cortez::memory::KernelHeap::instance().deallocate(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    cortez::memory::KernelHeap::instance().deallocate(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    cortez::memory::KernelHeap::instance().deallocate(ptr);
}
