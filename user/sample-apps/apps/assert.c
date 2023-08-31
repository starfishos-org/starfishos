#include <assert.h>
#include <stdio.h>

int main()
{
        assert(1);
        printf("assert(1) ok\n");
        assert(0);
        printf("assert(0) failed\n");
        return 0;
}
