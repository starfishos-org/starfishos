#include <common/stdarg.h>
#include <common/util.h>

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

static int intToStr(int x, char str[], int d)
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
                num = va_arg(args, int);
                intToStr(num, buffer, 0);
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
