#ifndef U__STR_UTILS_H
#define U__STR_UTILS_H

#include <stddef.h>
#include <stdint.h>

/**
 * Duplicate a string (C99 compatible alternative to strdup)
 * @param str: String to duplicate
 * @return: Newly allocated copy of string, NULL on failure
 * @note: Caller must free the returned string
 */
char* strduplic(const char* str);

/**
 * Duplicate a string with length limit
 * @param str: String to duplicate
 * @param max_len: Maximum number of characters to copy
 * @return: Newly allocated string, NULL on failure
 */
char* strdup(const char* str, size_t max_len);

/**
 * Check if string starts with specified prefix
 * @param str: String to check
 * @param prefix: Prefix to look for
 * @return: 1 if string starts with prefix, 0 otherwise
 */
int str_startw(const char* str, const char* prefix);

/**
 * Check if string ends with specified suffix
 * @param str: String to check
 * @param suffix: Suffix to look for
 * @return: 1 if string ends with suffix, 0 otherwise
 */
int str_endw(const char* str, const char* suffix);

/**
 * Check if two strings are equal
 * @param str1: First string
 * @param str2: Second string
 * @return: 1 if strings are equal, 0 otherwise
 */
int streq(const char* str1, const char* str2);

/**
 * Check if two strings are equal (case-insensitive)
 * @param str1: First string
 * @param str2: Second string
 * @return: 1 if strings are equal ignoring case, 0 otherwise
 */
int strequ_ignor(const char* str1, const char* str2);

/**
 * Copy string safely with buffer size limit
 * @param dest: Destination buffer
 * @param src: Source string
 * @param dest_size: Size of destination buffer
 * @return: Number of characters copied (excluding null terminator)
 */
size_t str_copy_safe(char* dest, const char* src, size_t dest_size);

/**
 * Concatenate strings safely with buffer size limit
 * @param dest: Destination buffer
 * @param src: Source string to append
 * @param dest_size: Size of destination buffer
 * @return: Number of characters in resulting string (excluding null terminator)
 */
size_t str_concat_safe(char* dest, const char* src, size_t dest_size);

/**
 * Trim leading and trailing whitespace from string
 * @param str: String to trim (modified in-place)
 * @return: Pointer to trimmed string (may be different from input)
 */
char* strtrim(char* str);

/**
 * Convert string to lowercase (in-place)
 * @param str: String to convert
 * @return: Pointer to converted string
 */
char* strlow(char* str);

/**
 * Convert string to uppercase (in-place)
 * @param str: String to convert
 * @return: Pointer to converted string
 */
char* strupp(char* str);

/**
 * Find first occurrence of character in string
 * @param str: String to search
 * @param ch: Character to find
 * @return: Pointer to first occurrence, NULL if not found
 */
const char* strfind_c(const char* str, char ch);

/**
 * Find last occurrence of character in string
 * @param str: String to search
 * @param ch: Character to find
 * @return: Pointer to last occurrence, NULL if not found
 */
const char* strfind_lc(const char* str, char ch);

/**
 * Check if string contains only whitespace characters
 * @param str: String to check
 * @return: 1 if string is empty or contains only whitespace, 0 otherwise
 */
int str_whitespace(const char* str);

#endif
