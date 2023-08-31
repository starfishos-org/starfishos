#include <stdio.h>
#include <chcore/pmu.h>

int main(int argc, char *argv[])
{
        s64 start = 0, end = 0;

        pmu_clear_cnt();
        start = pmu_read_real_cycle();
        printf("Hello.bin begin at cycle %ld\n", start);
        end = pmu_read_real_cycle();
        printf("Hello.bin end at cycle %ld\n", end);
        printf("Hello.bin total cycle %ld\n", end - start);

        return 0;
}
