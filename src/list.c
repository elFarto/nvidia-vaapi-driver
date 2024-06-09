#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"

static void ensure_capacity(Array *arr, uint32_t new_capacity) {
    if (new_capacity <= arr->capacity) {
        //we already have enough capacity to hold the new element
        return;
    }

    uint32_t old_capacity = arr->capacity;
    if (arr->capacity == 0) {
        //if we're completely empty allocate a small amount
        arr->capacity = 16;
    } else {
        //grow the capacity until we can hold the new amount
        while (new_capacity > arr->capacity) {
            arr->capacity += arr->capacity >> 1;
        }
    }

    arr->buf = realloc(arr->buf, arr->capacity * sizeof(void*));

    //clear the new part of the array
    memset(&arr->buf[old_capacity], 0, (arr->capacity - old_capacity) * sizeof(void*));
}

void add_element(Array *arr, void *element) {
    ensure_capacity(arr, arr->size + 1);

    arr->buf[arr->size++] = element;
}

void* alloc_and_add_element(Array *arr, size_t size) {
    ensure_capacity(arr, arr->size + 1);

    void *element = calloc(1, size);
    arr->buf[arr->size++] = element;
    return element;
}

void remove_element_at(Array *arr, uint32_t index) {
    if (index >= arr->size) {
        return;
    }

    arr->size--;
    if (index < arr->size) {
        for (uint32_t i = index; i < arr->size; i++) {
            arr->buf[i] = arr->buf[i+1];
        }
    }
    //clear out the remaining element
    arr->buf[arr->size] = NULL;
}

void remove_and_free_element_at(Array *arr, uint32_t index) {
    void *element = get_element_at(arr, index);
    remove_element_at(arr, index);
    free(element);
}

uint32_t get_size(Array *arr) {
    return arr->size;
}

void *get_element_at(Array *arr, uint32_t index) {
    if (arr->buf == NULL || index >= arr->size) {
        return NULL;
    }
    return arr->buf[index];
}
