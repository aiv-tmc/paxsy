#include "str_utils.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

char* u__strduplic(const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str);
    char* copy = malloc(len + 1);
    if (!copy) return NULL;
    
    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

char* u__strdup(const char* str, size_t max_len) {
    if (!str) return NULL;
    
    size_t len = 0;
    while (len < max_len && str[len]) len++;
    
    char* copy = malloc(len + 1);
    if (!copy) return NULL;
    
    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

char* u__strdup_safe(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* copy = (char*)memory_allocate_zero(len+1);
    if (copy) memcpy(copy, str, len+1);
    return copy;
}

int u__str_startw(const char* str, const char* prefix) {
    if (!str || !prefix) return 0;
    
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    return 1;
}

int u__str_endw(const char* str, const char* suffix) {
    if (!str || !suffix) return 0;
    
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    
    if (suffix_len > str_len) return 0;
    
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

int u__streq(const char* str1, const char* str2) {
    if (!str1 || !str2) return str1 == str2;
    return strcmp(str1, str2) == 0;
}

int u__streq_ignore(const char* str1, const char* str2) {
    if (!str1 || !str2) return str1 == str2;
    
    while (*str1 && *str2) {
        if ( tolower((unsigned char)*str1)
           != tolower((unsigned char)*str2)
        ) return 0;
        str1++;
        str2++;
    }
    
    return *str1 == *str2;
}

size_t u__str_copy_safe
    ( char* dest
    , const char* src
    , size_t dest_size
) {
    if (!dest || !src || dest_size == 0) return 0;
    
    size_t i = 0;
    while (i < dest_size - 1 && src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
    
    return i;
}

size_t u__str_concat_safe
    ( char* dest
    , const char* src
    , size_t dest_size
) {
    if (!dest || !src || dest_size == 0) return 0;
    
    size_t dest_len = strlen(dest);
    if (dest_len >= dest_size) return dest_len;
    
    size_t i = 0;
    while (dest_len + i < dest_size - 1 && src[i]) {
        dest[dest_len + i] = src[i];
        i++;
    }
    dest[dest_len + i] = '\0';
    
    return dest_len + i;
}

char* u__strtrim(char* str) {
    if (!str) return NULL;
    
    /* Trim leading whitespace */
    char* start = str;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    
    /* Trim trailing whitespace */
    char* end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1)))
        end--;
    *end = '\0';
    
    /* Move trimmed string to beginning if needed */
    if (start != str) {
        size_t len = end - start;
        memmove(str, start, len);
        str[len] = '\0';
    }
    
    return str;
}

char* u__strlow(char* str) {
    if (!str) return NULL;
    
    for (char* p = str; *p; p++)
        *p = tolower((unsigned char)*p);
    return str;
}

char* u__strupp(char* str) {
    if (!str) return NULL;
    
    for (char* p = str; *p; p++)
        *p = toupper((unsigned char)*p);
    return str;
}

const char* u__strfind_c(const char* str, char ch) {
    if (!str) return NULL;
    
    while (*str) {
        if (*str == ch) return str;
        str++;
    }
    
    return NULL;
}

const char* u__strfind_lc(const char* str, char ch) {
    if (!str) return NULL;
    
    const char* last = NULL;
    while (*str) {
        if (*str == ch) last = str;
        str++;
    }
    
    return last;
}

int u__str_whitespace(const char* str) {
    if (!str) return 1;
    
    while (*str) {
        if (!isspace((unsigned char)*str))
            return 0;
        str++;
    }
    
    return 1;
}
