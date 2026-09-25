// Prints CLOCK_MONOTONIC in milliseconds; the guest has no sub-second clock the shell can read.
#include <stdio.h>
#include <time.h>
int main(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);printf("%lld\n",(long long)t.tv_sec*1000+t.tv_nsec/1000000);return 0;}
