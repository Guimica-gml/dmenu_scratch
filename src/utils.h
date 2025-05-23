#ifndef UTILS_H_
#define UTILS_H_

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include "./arena.h"

#define SV(cstr) ((String_View) { .data = (cstr), .size = strlen(cstr) })
#define SV_STATIC(cstr) { .data = (cstr), .size = sizeof(cstr) - 1 }

#define str_append_char(a, str, ch) arena_da_append((a), (str), ch)
#define str_append_sv(a, str, sv) arena_da_append_many((a), (str), (sv).data, (sv).size)
#define str_append_cstr(a, str, cstr) arena_da_append_many((a), (str), (cstr), strlen(cstr))
#define str_append_null(a, str) arena_da_append((a), (str), '\0')

typedef struct {
    char *items;
    size_t count;
    size_t capacity;
} String;

String str_with_cap(Arena *arena, size_t cap);
bool str_eq(String *a, String *b);
bool str_eq_cstr(String *a, const char *b);
void str_append_vfmt(Arena *arena, String *str, const char *fmt, va_list args);
void str_append_fmt(Arena *arena, String *str, const char *fmt, ...);

typedef struct {
    const char *data;
    size_t size;
} String_View;

int64_t sv_to_int64(String_View sv);
double sv_to_decimal(String_View sv);

bool sv_find(String_View sv, char ch, size_t *index);
bool sv_find_rev(String_View sv, char ch, size_t *index);
bool sv_eq(String_View a, String_View b);

#endif // UTILS_H_
