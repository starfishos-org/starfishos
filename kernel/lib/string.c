#include <common/stdarg.h>
#include <common/util.h>

int is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

int is_hex_digit(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

int is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r';
}


static void reverse(char *str, int len)
{
    int i = 0, j = len - 1;
    while (i < j) {
        char temp = str[i];
        str[i] = str[j];
        str[j] = temp;
        i++;
        j--;
    }
}

static int digit_to_int(char ch) {
    return ch - '0';
}

static int hex_to_int(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/*
 * Convert an integer to a string representation
 * @x: The integer to convert
 * @str: The output string buffer
 * @d: Minimum number of digits (pad with leading zeros if needed)
 * @base: The base to use (e.g. 10 for decimal, 16 for hex)
 * Returns: The length of the resulting string
 */
static int int_to_str(int x, char str[], int d, int base)
{
    int i = 0;
    bool isNegative = false;

    if (x == 0) {
        str[i++] = '0';
    } else {
        if (x < 0) {
            isNegative = true;
            x = -x;
        }

        while (x) {
            int rem = x % base;
            str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
            x = x / base;
        }

        if (isNegative)
            str[i++] = '-';
    }

    while (i < d)
        str[i++] = '0';

    reverse(str, i);
    str[i] = '\0';
    return i;
}

int sscanf(const char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);

    int *int_val;
    char *str_val;
    int items_matched = 0;
    bool percent = false;

    while (*format) {
        if (*format == '%') {
            percent = true;
        } else if (percent) {
            percent = false;
            if (*format == 'd') {
                int_val = va_arg(args, int*);
                *int_val = 0;
                while (is_digit((unsigned char)*str)) {
                    *int_val = (*int_val) * 10 + digit_to_int(*str);
                    str++;
                }
                items_matched++;
            } else if (*format == 'x') {
                int_val = va_arg(args, int*);
                *int_val = 0;
                while (is_hex_digit((unsigned char)*str)) {
                    *int_val = (*int_val) * 16 + hex_to_int(*str);
                    str++;
                }
                items_matched++;
            } else if (*format == 's') {
                str_val = va_arg(args, char*);
                while (!is_space((unsigned char)*str) && *str) {
                    *str_val++ = *str++;
                }
                *str_val = '\0';
                items_matched++;
            }
        } else {
            if (*format != *str) break;
            str++;
        }
        format++;
    }

    va_end(args);
    return items_matched;
}

int snprintf(char *str, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);

    const char *traverse;
    char *s;
    char buffer[50];
    int len = 0;
    int num;

    for (traverse = format; *traverse != '\0'; traverse++) {
        if (*traverse == '%') {
            traverse++;

            switch (*traverse) {
            case 'd':
            case 'x':
                num = va_arg(args, int);
                int_to_str(num, buffer, 0, (*traverse == 'd') ? 10 : 16);
                for (s = buffer; *s != '\0'; s++, len++) {
                    if (len >= size) {
                        va_end(args);
                        return len;
                    }
                    str[len] = *s;
                }
                break;

            case 's':
                s = va_arg(args, char *);
                while (*s != '\0') {
                    if (len >= size) {
                        va_end(args);
                        return len;
                    }
                    str[len++] = *s++;
                }
                break;

            default:
                if (len >= size) {
                    va_end(args);
                    return len;
                }
                str[len++] = *traverse;
                break;
            }
        } else {
            if (len >= size) {
                va_end(args);
                return len;
            }
            str[len++] = *traverse;
        }
    }

    va_end(args);

    str[len] = '\0';
    return len;
}

const char *strstr(const char *haystack, const char *needle) {
    // if needle is empty, return haystack
    if (!*needle) {
        return haystack;
    }
    
    // main loop, traverse haystack
    for (const char *p = haystack; *p != '\0'; ++p) {
        // start matching
        const char *start = p, *pattern = needle;
        // if haystack is not end and needle is not end and character match, continue
        while (*start && *pattern && *start == *pattern) {
            ++start;
            ++pattern;
        }
        // if complete match needle, return current position
        if (!*pattern) {
            return p;
        }
    }
    
    // if not found, return NULL
    return NULL;
}
