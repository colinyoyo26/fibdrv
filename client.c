#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FIB_DEV "/dev/fibonacci"
#define KOBJ "cat /sys/kernel/kobj_ref/kt_ns"

long long get_ktime()
{
    FILE *kobj = popen(KOBJ, "r");
    if (!kobj)
        return -1;

    long long kt_ns = 0;
    if (!fscanf(kobj, "%lld\n", &kt_ns))
        kt_ns = -2;

    fclose(kobj);
    return kt_ns;
}

static inline long long get_nanotime()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main()
{
    char buf[10000];
    char write_buf[] = "testing writing";
    int offset = 100; /* TODO: try test something bigger than the limit */

    int fd = open(FIB_DEV, O_RDWR);
    FILE *data = fopen("data.txt", "w");

    if (fd < 0) {
        perror("Failed to open character device");
        exit(1);
    }

    if (!data) {
        perror("Failed to open data text");
        exit(2);
    }

    for (int i = 0; i <= offset; i++) {
        long long sz = write(fd, write_buf, strlen(write_buf));
        printf("Writing to " FIB_DEV ", returned the sequence %lld\n", sz);
    }

    for (int i = 0; i <= offset; i++) {
        lseek(fd, i, SEEK_SET);
        long long start = get_nanotime();
        read(fd, buf, 1);
        long long utime = get_nanotime() - start;
        long long ktime = get_ktime();
        fprintf(data, "%d %lld %lld %lld\n", i, ktime, utime, utime - ktime);
        printf("Reading from " FIB_DEV
               " at offset %d, returned the sequence "
               "%s.\n",
               i, buf);
    }

    for (int i = offset; i >= 0; i--) {
        lseek(fd, i, SEEK_SET);
        read(fd, buf, 1);
        printf("Reading from " FIB_DEV
               " at offset %d, returned the sequence "
               "%s.\n",
               i, buf);
    }

    close(fd);
    fclose(data);
    return 0;
}
