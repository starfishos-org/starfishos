#include <stdio.h>
#include <chcore/syscall.h>

#define X86_TIMER_IRQ      0x20

int main(int argc, char *argv[])
{
        int irq_cap;

        irq_cap = usys_irq_register(X86_TIMER_IRQ);
        while (1) {
                printf("Waiting irq\n");
                usys_irq_wait(irq_cap, true);
                printf("Waked up. Acking irq\n");
                usys_irq_ack(irq_cap);
        }

        return 0;
}
