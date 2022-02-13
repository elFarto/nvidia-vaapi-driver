#include <stdint.h>
#include <stddef.h>

typedef struct {
    void **buf;
    uint32_t size;
    uint32_t capacity;
} Array;

//maybe not the nicest, but simple. Usage:
//    ARRAY_FOR_EACH(void*, it, arr)
//        printf("%u: %p\n", it_idx, it);
//    END_FOR_EACH
#define ARRAY_FOR_EACH(T, N, A) for (uint32_t N ## _idx = 0; N ## _idx < (A)->size; N ## _idx++) { T N = (T) (A)->buf[N ## _idx];
#define ARRAY_FOR_EACH_REV(T, N, A) for (uint32_t N ## _idx = (A)->size-1; N ## _idx < (A)->size; N ## _idx--) { T N = (T) (A)->buf[N ## _idx];
#define END_FOR_EACH }

void add_element(Array *arr, void *element);

void remove_element_at(Array *arr, uint32_t index);

uint32_t get_size(Array *arr);

void *get_element_at(Array *arr, uint32_t index);

void remove_and_free_element_at(Array *arr, uint32_t index);

void* alloc_and_add_element(Array *arr, size_t size);
