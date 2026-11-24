// Payload (sent to elfldr, port 9021): kills the app whose AppId is given at build time (-DKILL_APP_ID=0x...), reports to klog.
// A title process ignores the shell's kill; tools/ps5/run_title.sh builds and sends this to stop a running or hung title.
#include <stdio.h>
int sceLncUtilKillApp(int app_id);
int sceLncUtilForceKillApp(int app_id);
int sceKernelDebugOutText(int channel, const char *text);
int main(void) {
    char line[120];
    int rc = sceLncUtilKillApp(KILL_APP_ID);
    snprintf(line, sizeof line, "[killapp] sceLncUtilKillApp(%#x) = %#x\n", KILL_APP_ID, rc);
    sceKernelDebugOutText(0, line);
    if (rc) {
        rc = sceLncUtilForceKillApp(KILL_APP_ID);
        snprintf(line, sizeof line, "[killapp] sceLncUtilForceKillApp(%#x) = %#x\n", KILL_APP_ID, rc);
        sceKernelDebugOutText(0, line);
    }
    return 0;
}
