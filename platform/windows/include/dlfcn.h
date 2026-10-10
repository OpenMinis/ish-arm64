#define RTLD_DEFAULT ((void *) 0)
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
void *px_dlsym(void *handle, const char *name);
#define dlsym px_dlsym
