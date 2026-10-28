#include "heap.h"
#include "../kernel/panic.h"

// ─── Пул кучи: статический массив внутри BSS ядра ────────────────────────────
// 16 МБ — с запасом под текущие нужды (буфер обмена, будущие ELF-программы),
// но не настолько много, чтобы бездумно раздувать образ ядра. Легко поднять
// одной цифрой здесь, если понадобится больше.
#define HEAP_SIZE (16u * 1024u * 1024u)
static uint8_t heap_pool[HEAP_SIZE];

#define BLOCK_MAGIC 0xB10CB10Cu   // "BLOCblOC" — канарейка живого блока
#define ALIGN       8u

struct BlockHeader {
    uint32_t magic;      // должно быть BLOCK_MAGIC, пока блок используется/цел
    size_t   size;        // размер ПОЛЕЗНОЙ области (без этого заголовка)
    bool     free;
    BlockHeader* next;
    BlockHeader* prev;
};

static BlockHeader* heap_head = nullptr;
static bool heap_ready = false;

static size_t align_up(size_t n) { return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1); }

void heap_init() {
    heap_head = (BlockHeader*)heap_pool;
    heap_head->magic = BLOCK_MAGIC;
    heap_head->size  = HEAP_SIZE - sizeof(BlockHeader);
    heap_head->free  = true;
    heap_head->next  = nullptr;
    heap_head->prev  = nullptr;
    heap_ready = true;
}

void* kmalloc(size_t size) {
    if (!heap_ready) heap_init();
    if (size == 0) return nullptr;
    size = align_up(size);

    BlockHeader* b = heap_head;
    while (b) {
        if (b->magic != BLOCK_MAGIC) panic("Heap corrupted: bad block magic (kmalloc walk)");
        if (b->free && b->size >= size) {
            // Делим блок, только если остаток достаточно большой, чтобы иметь
            // смысл как отдельный блок (заголовок + хоть немного полезного места).
            if (b->size >= size + sizeof(BlockHeader) + ALIGN) {
                BlockHeader* rem = (BlockHeader*)((uint8_t*)b + sizeof(BlockHeader) + size);
                rem->magic = BLOCK_MAGIC;
                rem->size  = b->size - size - sizeof(BlockHeader);
                rem->free  = true;
                rem->next  = b->next;
                rem->prev  = b;
                if (b->next) b->next->prev = rem;
                b->next = rem;
                b->size = size;
            }
            b->free = false;
            return (uint8_t*)b + sizeof(BlockHeader);
        }
        b = b->next;
    }
    return nullptr; // не нашли достаточно большой свободный блок
}

void kfree(void* ptr) {
    if (!ptr) return;
    BlockHeader* b = (BlockHeader*)((uint8_t*)ptr - sizeof(BlockHeader));
    if (b->magic != BLOCK_MAGIC) panic("Heap corrupted: kfree() on invalid or already-freed pointer");
    if (b->free) panic("Heap corrupted: double free");
    b->free = true;
    // Сращивание со следующим свободным соседом
    if (b->next && b->next->free) {
        if (b->next->magic != BLOCK_MAGIC) panic("Heap corrupted: bad magic on neighbor (kfree merge next)");
        b->size += sizeof(BlockHeader) + b->next->size;
        BlockHeader* dead = b->next;
        b->next = dead->next;
        if (b->next) b->next->prev = b;
        dead->magic = 0; // гасим канарейку освобождённого заголовка — случайный повторный доступ поймается сразу
    }
    // Сращивание с предыдущим свободным соседом
    if (b->prev && b->prev->free) {
        if (b->prev->magic != BLOCK_MAGIC) panic("Heap corrupted: bad magic on neighbor (kfree merge prev)");
        b->prev->size += sizeof(BlockHeader) + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
        b->magic = 0;
    }
}

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return nullptr; }
    BlockHeader* b = (BlockHeader*)((uint8_t*)ptr - sizeof(BlockHeader));
    if (b->magic != BLOCK_MAGIC) panic("Heap corrupted: krealloc() on invalid pointer");
    new_size = align_up(new_size);
    if (b->size >= new_size) return ptr; // уже хватает места — ничего не трогаем
    void* np = kmalloc(new_size);
    if (!np) return nullptr;
    size_t copy_n = b->size < new_size ? b->size : new_size;
    uint8_t* d = (uint8_t*)np;
    uint8_t* s = (uint8_t*)ptr;
    for (size_t i = 0; i < copy_n; i++) d[i] = s[i];
    kfree(ptr);
    return np;
}

size_t heap_total() { return HEAP_SIZE; }

size_t heap_used() {
    if (!heap_ready) heap_init();
    size_t used = 0;
    for (BlockHeader* b = heap_head; b; b = b->next)
        if (!b->free) used += b->size;
    return used;
}

size_t heap_free() {
    if (!heap_ready) heap_init();
    size_t f = 0;
    for (BlockHeader* b = heap_head; b; b = b->next)
        if (b->free) f += b->size;
    return f;
}

bool heap_check() {
    if (!heap_ready) heap_init();
    size_t total = 0;
    for (BlockHeader* b = heap_head; b; b = b->next) {
        if (b->magic != BLOCK_MAGIC) return false;
        total += sizeof(BlockHeader) + b->size;
    }
    return total == HEAP_SIZE;
}

// ─── стандартные имена ────────────────────────────────────────────────────
extern "C" void* malloc(size_t size)             { return kmalloc(size); }
extern "C" void  free(void* ptr)                 { kfree(ptr); }
extern "C" void* realloc(void* ptr, size_t size) { return krealloc(ptr, size); }
