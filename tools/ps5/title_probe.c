// Throwaway title: what can a native PS5 title see, and does it start when eboot.bin is big?
// Padded by PAD_MIB (compile with -DPAD_MIB=380). Results go to klog and to /app0/title-probe.txt.
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int sceKernelDebugOutText(int channel, const char *text);
int sceKernelUsleep(unsigned int microseconds);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);

// title_probe_pad.S: PAD_MIB MiB of non-zero read-only data, made by the assembler
extern const unsigned char pad[], pad_end[];

static FILE *results;

static void say(const char *format, ...)
{
    char line[400];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    char klog[420];
    snprintf(klog, sizeof klog, "[title-probe] %s\n", line);
    sceKernelDebugOutText(0, klog);
    if (results) {
        fprintf(results, "%s\n", line);
        fflush(results);
    }
}

static void probe_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        say("stat %-48s ok  mode %o size %lld", path, st.st_mode & 07777, (long long)st.st_size);
    else
        say("stat %-48s ERR errno %d (%s)", path, errno, strerror(errno));
}

static void probe_read(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        say("open %-48s ERR errno %d (%s)", path, errno, strerror(errno));
        return;
    }
    char head[48] = {0};
    ssize_t n = read(fd, head, sizeof head - 1);
    for (ssize_t i = 0; i < n; i++)
        if (head[i] < 32 || head[i] > 126) head[i] = '.';
    say("open %-48s ok  read %zd bytes: %s", path, n, head);
    close(fd);
}

static void probe_write(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        say("write %-47s ERR errno %d (%s)", path, errno, strerror(errno));
        return;
    }
    ssize_t n = write(fd, "probe\n", 6);
    close(fd);
    unlink(path);
    say("write %-47s ok  wrote %zd bytes", path, n);
}

int main(void)
{
    results = fopen("/app0/title-probe.txt", "w");
    say("title probe starts; padding %ld MiB (first %d last %d)", (long)((pad_end - pad) >> 20), pad[0], pad_end[-1]);
    static const char *const paths[] = {
        "/app0", "/app0/eboot.bin", "/download0", "/data", "/data/homebrew", "/data/portal2", "/data/portal2/game",
        "/data/portal2/game/portal2/gameinfo.txt", "/data/portal2/log.txt", "/data/homebrew/PPSA99014/eboot.bin",
        "/mnt/usb0", "/system_data", "/user", "/av_contents", "/mnt/sandbox",
    };
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++)
        probe_path(paths[i]);
    probe_read("/data/portal2/game/portal2/gameinfo.txt");
    probe_read("/data/portal2/log.txt");
    probe_write("/app0/probe-write.tmp");
    probe_write("/download0/probe-write.tmp");
    probe_write("/data/portal2/probe-write.tmp");
    say("title probe ends");
    return 0;
}

// The title ends by asking the shell to close it: returning from _start or exit() is a SIGSYS the console reports as a
// crash. The CRT calls this after main (a weak hook); the shell closes the title asynchronously, so it never returns.
void catchReturnFromMain(int status)
{
    say("exit status %d: asking the shell to close the title", status);
    if (results) {
        fclose(results);
        results = NULL;
    }
    (void)sceSystemServiceLoadExec("exit", NULL);
    for (;;)
        sceKernelUsleep(100000);
}
