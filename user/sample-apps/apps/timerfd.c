#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <chcore/pmu.h>
#include <chcore/bug.h>

int main()
{
        int timerfd, timerfd_nb;
        struct itimerspec new_value, curr_value;
        struct timespec time;
        uint64_t counter;
        int ret;

        timerfd = timerfd_create(CLOCK_MONOTONIC, 0);

        /* set start timer to 4s later */
        new_value.it_value.tv_sec = 4;
        new_value.it_value.tv_nsec = 0;
        new_value.it_interval.tv_sec = 1;
        new_value.it_interval.tv_nsec = 0;
        timerfd_settime(timerfd, 0, &new_value, NULL);

        /* read 8s later */
        sleep(8);
        ret = read(timerfd, &counter, sizeof(counter));
        printf("Read return %d %d\n", ret, errno);
        BUG_ON(ret < 0);
        printf("count %ld\n", counter);

        /* read 3s later */
        sleep(3);
        ret = read(timerfd, &counter, sizeof(counter));
        BUG_ON(ret < 0);
        printf("count %ld\n", counter);

        /* set start timer to 4s later and directly read */
        new_value.it_value.tv_sec = 4;
        new_value.it_value.tv_nsec = 0;
        new_value.it_interval.tv_sec = 1;
        new_value.it_interval.tv_nsec = 0;
        timerfd_settime(timerfd, 0, &new_value, NULL);

        ret = read(timerfd, &counter, sizeof(counter));
        BUG_ON(ret < 0);
        printf("count %ld\n", counter);

        timerfd_gettime(timerfd, &curr_value);
        ret = close(timerfd);
        BUG_ON(ret < 0);

        /* Non blocking timerfd */
        timerfd_nb = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

        /* TFD_TIMER_ABSTIME mode */
        clock_gettime(CLOCK_MONOTONIC, &time);
        new_value.it_value.tv_sec = 4 + time.tv_sec;
        new_value.it_value.tv_nsec = time.tv_nsec;
        new_value.it_interval.tv_sec = 1;
        new_value.it_interval.tv_nsec = 0;
        timerfd_settime(timerfd_nb, TFD_TIMER_ABSTIME, &new_value, NULL);

        ret = read(timerfd_nb, &counter, sizeof(counter));
        printf("ret:%d errno:%d\n", ret, errno);

        sleep(8);
        ret = read(timerfd_nb, &counter, sizeof(counter));
        BUG_ON(ret < 0);
        printf("count %ld\n", counter);
        ret = close(timerfd_nb);
        BUG_ON(ret < 0);
        printf("timerfd finished!\n");

        return 0;
}
