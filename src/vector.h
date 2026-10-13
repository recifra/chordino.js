#ifndef CHORDINO_VECTOR_H
#define CHORDINO_VECTOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A growing array of elements of one size, what std::vector was for the C++
// this was ported from. It is held by value, inside the struct that uses it;
// a static one can start as {.size = sizeof(element)} instead of
// vector_create. data moves when the vector grows.
typedef struct {
    void *data;
    int count;
    int capacity;
    size_t size;            // of an element, in bytes
} vector_t;

vector_t vector_create(size_t size);
void vector_destroy(vector_t *vector);
// Empties it, keeping the memory for what comes next
void vector_clear(vector_t *vector);
// Makes it count elements long, the new ones zeros; returns data
void *vector_resize(vector_t *vector, int count);
// Adds count elements copied from values (zeros when values is null) at index
// at, moving those from there up; returns the first one added, or null when
// count is 0
void *vector_insert(vector_t *vector, int at, const void *values, int count);
// vector_insert at the end
void *vector_push(vector_t *vector, const void *values, int count);
// Removes count elements from index at, moving those after them down
void vector_erase(vector_t *vector, int at, int count);

#ifdef __cplusplus
}
#endif

#endif
