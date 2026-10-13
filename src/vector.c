#include "vector.h"

#include <stdlib.h>
#include <string.h>

// Kept out of line, where marked: -O3 inlined these into every caller, which
// made the wasm 1.3 KB bigger.
#define OUT_OF_LINE __attribute__((noinline))

vector_t vector_create(size_t size) {
    return (vector_t) {NULL, 0, 0, size};
}

OUT_OF_LINE void vector_destroy(vector_t *vector) {
    free(vector->data);
    *vector = vector_create(vector->size);
}

void vector_clear(vector_t *vector) {
    vector->count = 0;
}

// Makes room for count elements, doubling the capacity
static void reserve(vector_t *vector, int count) {
    if (count <= vector->capacity) return;
    int grown = vector->capacity > 0 ? vector->capacity : 16;
    while (grown < count) grown *= 2;
    vector->data = realloc(vector->data, grown * vector->size);
    vector->capacity = grown;
}

OUT_OF_LINE void *vector_resize(vector_t *vector, int count) {
    reserve(vector, count);
    char *data = vector->data;
    if (count > vector->count) {
        memset(data + vector->count * vector->size, 0, (count - vector->count) * vector->size);
    }
    vector->count = count;
    return data;
}

OUT_OF_LINE void *vector_insert(vector_t *vector, int at, const void *values, int count) {
    if (count == 0) return NULL;
    reserve(vector, vector->count + count);
    char *added = (char *) vector->data + at * vector->size;
    memmove(added + count * vector->size, added, (vector->count - at) * vector->size);
    if (values) memcpy(added, values, count * vector->size);
    else memset(added, 0, count * vector->size);
    vector->count += count;
    return added;
}

void *vector_push(vector_t *vector, const void *values, int count) {
    return vector_insert(vector, vector->count, values, count);
}

OUT_OF_LINE void vector_erase(vector_t *vector, int at, int count) {
    if (count == 0) return;
    char *removed = (char *) vector->data + at * vector->size;
    memmove(removed, removed + count * vector->size, (vector->count - at - count) * vector->size);
    vector->count -= count;
}
