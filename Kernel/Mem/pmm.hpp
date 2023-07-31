#ifndef PMM_HPP
#define PMM_HPP

#include "paging.hpp"
#include <LibC++/bitarray.hpp>
#include <LibC/mem.h>
#include <LibC/stdlib.h>

#define MAX_PAGES 488280 /* around 2 gigabytes */
#define PHYSICAL_MEMORY_START PAGE_ALIGN(10 * MB)
#define PMM PhysicalMemoryManager::active

class PhysicalMemoryManager {
private:
    bool debug = false;
    uint32_t available_pages = 0;
    uint32_t used_pages = 0;

public:
    PhysicalMemoryManager(uint32_t pages);
    ~PhysicalMemoryManager();

    static PhysicalMemoryManager* active;

    void stat(uint32_t* free_pages, uint32_t* used_pages);
    uint32_t allocate_pages(size_t size);
    int free_pages(uint32_t address, size_t size);
    void info();
};

#endif
