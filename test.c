#include<sys/stat.h>
#include<stdio.h>
#include<sys/types.h>

struct s {
    __mode_t a;
    __u_int x;
    __time_t f;
    __time_t g;
    __time_t h;
    __u_short e;
    __u_short y;
    __u_short k[15];
}aaa;

int main() {
    printf("%ld\n",sizeof(aaa));
    return 0;
}
