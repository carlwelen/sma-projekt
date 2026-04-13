//This was heavily built on "A Type System for Safe Region-Based Memory Management in Real-Time Java" by Alexandru Salcianu, Chandrasekhar Boyapati, William Beebee, Jr., Martin Rinard, MIT Laboratory for Computer Science.

//This project was built in order to practice vim-motions, so the code may be janky and inefficient.


#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define KiB(n) ((uint64_t)(n) << 10)
#define MiB(n) ((uint64_t)(n) << 20)
#define GiB(n) ((uint64_t)(n) << 30)

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define ALIGN_UP_POW2(n, p) (((uint64_t)(n) + ((uint64_t)(p) - 1)) & (~((uint64_t)(p) - 1)))

#define ARENA_BASE_POS (sizeof(mem_arena))
#define ARENA_ALIGN (sizeof(void*))

typedef struct {
    uint64_t reserve_size;
    uint64_t commit_size;

    uint64_t pos;
    uint64_t commit_pos;
} mem_arena;

typedef struct {
    mem_arena* arena;
    uint64_t start_pos;
} mem_arena_temp;

mem_arena* arena_create(uint64_t reserve_size, uint64_t commit_size);
void arena_destroy(mem_arena* arena);
void* arena_push(mem_arena* arena, uint64_t size, int32_t non_zero);
void arena_pop(mem_arena* arena, uint64_t size);
void arena_pop_to(mem_arena* arena, uint64_t pos);
void arena_clear(mem_arena* arena);

mem_arena_temp arena_temp_begin(mem_arena* arena);
void arena_temp_end(mem_arena_temp temp);

mem_arena_temp arena_scratch_get(mem_arena** conflicts, uint32_t num_conflicts);
void arena_scratch_release(mem_arena_temp scratch);

#define PUSH_STRUCT(arena, T) (T*)arena_push((arena), sizeof(T), false)
#define PUSH_STRUCT_NZ(arena, T) (T*)arena_push((arena), sizeof(T), true)
#define PUSH_ARRAY(arena, T, n) (T*)arena_push((arena), sizeof(T) * (n), false)
#define PUSH_ARRAY_NZ(arena, T, n) (T*)arena_push((arena), sizeof(T) * (n), true)

uint32_t plat_get_pagesize(void);


void* plat_mem_reserve(uint64_t size);
int32_t plat_mem_commit(void* ptr, uint64_t size);
int32_t plat_mem_decommit(void* ptr, uint64_t size);
int32_t plat_mem_release(void* ptr, uint64_t size);

int main(void) {
    mem_arena* perm_arena = arena_create(GiB(1), MiB(1));

    while (1) {
        arena_push(perm_arena, MiB(16), false);
        getc(stdin);
    }

    arena_destroy(perm_arena);

    return 0;
}

mem_arena* arena_create(uint64_t reserve_size, uint64_t commit_size) {
    uint32_t pagesize = plat_get_pagesize();

    reserve_size = ALIGN_UP_POW2(reserve_size, pagesize);
    commit_size = ALIGN_UP_POW2(commit_size, pagesize);

    mem_arena* arena = plat_mem_reserve(reserve_size);

    if (!plat_mem_commit(arena, commit_size)) {
        return NULL;
    }

    arena->reserve_size = reserve_size;
    arena->commit_size = commit_size;
    arena->pos = ARENA_BASE_POS;
    arena->commit_pos = commit_size;
    
    return arena;
}

void arena_destroy(mem_arena* arena) {
    plat_mem_release(arena, arena->reserve_size);
}

void* arena_push(mem_arena* arena, uint64_t size, int32_t non_zero) {
    uint64_t pos_aligned = ALIGN_UP_POW2(arena->pos, ARENA_ALIGN);
    uint64_t new_pos = pos_aligned + size;

    if (new_pos > arena->reserve_size) { return NULL; }

    if (new_pos > arena->commit_pos) {
        uint64_t new_commit_pos = new_pos;
        new_commit_pos += arena->commit_size - 1;
        new_commit_pos -= new_commit_pos % arena->commit_size;
        new_commit_pos = MIN(new_commit_pos, arena->reserve_size);

        uint8_t* mem = (uint8_t*)arena + arena->commit_pos;
        uint64_t commit_size = new_commit_pos - arena->commit_pos;

        if (!plat_mem_commit(mem, commit_size)) {
            return NULL;
        }

        arena->commit_pos = new_commit_pos;
    }

    arena->pos = new_pos;

    uint8_t* out = (uint8_t*)arena + pos_aligned;

    if (!non_zero) {
        memset(out, 0, size);
    }

    return out;
}

void arena_pop(mem_arena* arena, uint64_t size) {
    size = MIN(size, arena->pos - ARENA_BASE_POS);
    arena->pos -= size;
}

void arena_pop_to(mem_arena* arena, uint64_t pos) {
    uint64_t size = pos < arena->pos ? arena->pos - pos : 0;
    arena_pop(arena, size);
}

void arena_clear(mem_arena* arena) {
    arena_pop_to(arena, ARENA_BASE_POS);
}

mem_arena_temp arena_temp_begin(mem_arena* arena) {
    return (mem_arena_temp) {
        .arena = arena,
        .start_pos = arena->pos
    };
}

void arena_temp_end(mem_arena_temp temp) {
    arena_pop_to(temp.arena, temp.start_pos);
}

static __thread mem_arena* _scratch_arenas[2] = { NULL, NULL };

mem_arena_temp arena_scratch_get(mem_arena** conflicts, uint32_t num_conflicts) {
    int32_t scratch_index = -1;

    for (int32_t i = 0; i < 2; i++) {
        int32_t conflict_found = false;

        for (uint32_t j = 0; j < num_conflicts; j++) {
            if (_scratch_arenas[i] == conflicts[j]) {
                conflict_found = true;
                break;
            }
        }

        if (!conflict_found) {
            scratch_index = i;
            break;
        }
    }

    if (scratch_index == -1) {
        return (mem_arena_temp){ 0 };
    }

    mem_arena** selected = &_scratch_arenas[scratch_index];

    if (*selected == NULL) {
        *selected = arena_create(MiB(64), MiB(1));
    }

    return arena_temp_begin(*selected);
}

void arena_scratch_release(mem_arena_temp scratch) {
    arena_temp_end(scratch);
}

#if defined(_WIN32)

#include <windows.h>

uint32_t plat_get_pagesize(void) {
    SYSTEM_INFO sysinfo = { 0 };
    GetSystemInfo(&sysinfo);

    return sysinfo.dwPageSize;
}

void* plat_mem_reserve(uint64_t size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
}

int32_t plat_mem_commit(void* ptr, uint64_t size) {
    void* ret = VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE);
    return ret != NULL;
}

int32_t plat_mem_decommit(void* ptr, uint64_t size) {
    return VirtualFree(ptr, size, MEM_DECOMMIT);
}

int32_t plat_mem_release(void* ptr, uint64_t size) {
    return VirtualFree(ptr, size, MEM_RELEASE);
}


#elif defined(__linux__)

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <unistd.h>
#include <sys/mman.h>

uint32_t plat_get_pagesize(void) {
    return (uint32_t)sysconf(_SC_PAGESIZE);
}

void* plat_mem_reserve(uint64_t size) {
    void* out = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (out == MAP_FAILED) {
        return NULL;
    }
    return out;
}

int32_t plat_mem_commit(void* ptr, uint64_t size) {
    int32_t ret = mprotect(ptr, size, PROT_READ | PROT_WRITE);
    return ret == 0;
}

int32_t plat_mem_decommit(void* ptr, uint64_t size) {
    int32_t ret = mprotect(ptr, size, PROT_NONE);
    if (ret != 0) return false;
    ret = madvise(ptr, size, MADV_DONTNEED);
    return ret == 0;
}

int32_t plat_mem_release(void* ptr, uint64_t size) {
    int32_t ret = munmap(ptr, size);
    return ret == 0;
}

#endif


