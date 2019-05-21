#include<sys/stat.h>
#include<stdio.h>

struct s {
    __mode_t a;
    __nlink_t b;
    __uid_t c;
    __gid_t d;
    __off_t e;
    __time_t f;
    __time_t g;
    __time_t h;
}aaa;

int main() {
    printf("%ld\n",sizeof(aaa));
    return 0;
}