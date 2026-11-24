#include "app.h"

/* ---------------------------------------------------------------- system plumbing */
int exit_callback(int a, int b, void *c) { sceKernelExitGame(); return 0; }

int power_callback(int unknown, int pwrflags, void *common) {
    if (pwrflags & PSP_POWER_CB_RESUME_COMPLETE) g_resumed = 1;      /* woke from sleep: Wi-Fi must be rejoined */
    return 0;
}

int cb_thread(SceSize args, void *argp) {
    int cb = sceKernelCreateCallback("exit_callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    int pcb = sceKernelCreateCallback("power_callback", power_callback, NULL);
    scePowerRegisterCallback(0, pcb);
    sceKernelSleepThreadCB();
    return 0;
}

void setup_callbacks(void) {
    int t = sceKernelCreateThread("update_thread", cb_thread, 0x11, 0xFA0, 0, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
}
