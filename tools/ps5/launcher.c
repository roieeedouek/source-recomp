// PS5 launcher: a small ELF for the payload loader. It loads libvulkan (and the GPU driver behind it) while the process is
// still small, then loads the game as a shared library and runs its main(). Loading libvulkan after the 386 MB game image
// is mapped does not work on the console (docs/ps5.md). Build: see tools/package_ps5.py.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ROOT
#define ROOT "/data/portal2"
#endif

typedef struct { char useless1[45]; char message[3075]; } notify_request;
int sceKernelSendNotificationRequest(int, notify_request *, size_t, int);
static void notify(const char *text) {
    notify_request r;
    memset(&r, 0, sizeof r);
    snprintf(r.message, sizeof r.message, "%s", text);
    sceKernelSendNotificationRequest(0, &r, sizeof r, 0);
}

int main(void) {
    mkdir(ROOT, 0777);
    if (!freopen(ROOT "/log.txt", "w", stderr)) notify("portal2 launcher: cannot open log");
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(fileno(stderr), 1);
    setvbuf(stdout, NULL, _IONBF, 0);
    notify("portal2: launcher started");

    void *vulkan = dlopen(ROOT "/lib/libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "launcher: libvulkan -> %p %s\n", vulkan, vulkan ? "ok" : dlerror());
    if (!vulkan) { notify("portal2: cannot load libvulkan"); return 1; }
    {   // diagnostics for SDL, which opens the same path with other flags
        static const struct { const char *name; int flags; } variants[] = {
            { "RTLD_NOW|RTLD_LOCAL", RTLD_NOW | RTLD_LOCAL }, { "RTLD_LAZY", RTLD_LAZY }, { "RTLD_NOW|RTLD_GLOBAL", RTLD_NOW | RTLD_GLOBAL },
        };
        for (unsigned i = 0; i < sizeof variants / sizeof *variants; ++i) {
            void *again = dlopen(ROOT "/lib/libvulkan.so.1", variants[i].flags);
            const char *error = again ? "" : dlerror();
            fprintf(stderr, "launcher: dlopen again (%s) -> %p %s\n", variants[i].name, again, error ? error : "(no error text)");
        }
        void *by_name = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
        fprintf(stderr, "launcher: dlopen by name -> %p\n", by_name);
    }

    { char text[32]; snprintf(text, sizeof text, "%llx", (unsigned long long)(unsigned long)vulkan); setenv("P2_VULKAN_HANDLE", text, 1); }
    void *game = dlopen(ROOT "/portal2-ps5.so", RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "launcher: game library -> %p %s\n", game, game ? "ok" : dlerror());
    if (!game) { notify("portal2: cannot load the game library"); return 1; }

    void (*set_dl)(void *, void *, void *, void *) = (void (*)(void *, void *, void *, void *))dlsym(game, "rt_ps5_set_dl");
    if (set_dl) set_dl((void *)dlopen, (void *)dlsym, (void *)dlerror, (void *)dlclose);
    fprintf(stderr, "launcher: dl functions registered: %s\n", set_dl ? "yes" : "NO (rt_ps5_set_dl missing)");
    int (*game_main)(int, char **) = (int (*)(int, char **))dlsym(game, "main");
    if (!game_main) { fprintf(stderr, "launcher: main not found: %s\n", dlerror()); return 1; }
    char *argv[] = { "portal2", NULL };
    fprintf(stderr, "launcher: calling main\n");
    return game_main(1, argv);
}
