#include <common/stdarg.h>
#include <common/util.h>

static void reverse(char *str, int len) {
    int i = 0, j = len - 1;
    while (i < j) {
        char temp = str[i];
        str[i] = str[j];
        str[j] = temp;
        i++;
        j--;
    }
}

static int intToStr(int x, char str[], int d) {
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
            int rem = x % 10;
            str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
            x = x / 10;
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

static int snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);

    const char *traverse;
    char *s;
    char buffer[50];
    int i = 0;
    int len = 0;

    for (traverse = format; *traverse != '\0' && len < size - 1; traverse++) {
        while (*traverse != '%' && *traverse != '\0' && len < size - 1) {
            str[len++] = *traverse++;
        }

        if (*traverse == '\0' || len >= size - 1)
            break;

        traverse++;

        switch (*traverse) {
            case 's':
                s = va_arg(args, char *);
                while (*s && len < size - 1) {
                    str[len++] = *s++;
                }
                break;
            case 'd':
                i = intToStr(va_arg(args, int), buffer, 0);
                for (int j = 0; buffer[j] != '\0' && len < size - 1; j++) {
                    str[len++] = buffer[j];
                }
                break;
            case 'x':
                // This is a very simple implementation and does not handle all edge cases.
                i = intToStr(va_arg(args, int), buffer, 0);
                for (int j = 0; buffer[j] != '\0' && len < size - 1; j++) {
                    str[len++] = buffer[j];
                }
                break;
        }
    }

    va_end(args);

    str[len] = '\0';
    return len;
}
