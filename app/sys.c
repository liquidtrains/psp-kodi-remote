#include "app.h"

/* ---------------------------------------------------------------- system plumbing */
volatile int g_blanked;
static int g_bright = 60;
#ifdef ARK_EXTRAS
/* A normal user-mode EBOOT cannot read the music-note button or set the backlight. ARK-4's SystemControl lets it look up a kernel function by name
 * (sctrlHENFindFunction) and call it with kernel rights (kuKernelCall). Nothing here runs unless every lookup returned an address. */
typedef struct { unsigned arg[12]; unsigned ret1, ret2; } KuArgs;
int kuKernelCall(void *func_addr, KuArgs *args);
unsigned sctrlHENFindFunction(const char *mod_name, const char *library, unsigned nid);
volatile int g_ark_ok;
int g_dark_level;
static unsigned fn_ctrl, fn_set, fn_get;
static SceCtrlData g_kpad __attribute__((aligned(64)));
static int g_kbuf[2] __attribute__((aligned(64)));

int ark_init(void) {
    char m[100];
    fn_ctrl = sctrlHENFindFunction("sceController_Service", "sceCtrl_driver", 0x3A622550);
    fn_set = sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", 0x9E3C6DC6);
    fn_get = sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", 0x31C4BAA8);
    snprintf(m, sizeof(m), "ark lookup: ctrl %08x  set %08x  get %08x", fn_ctrl, fn_set, fn_get);
    logf_("%s", m);
    g_ark_ok = fn_ctrl && fn_set && fn_get;
    return g_ark_ok;
}
unsigned ark_buttons(void) {                              /* all buttons, extra ones included; 0 if the call is not available */
    KuArgs a;
    SceCtrlData *u = (SceCtrlData *)((unsigned)&g_kpad | 0x40000000);          /* uncached, so the kernel's write is what we read */
    memset(&a, 0, sizeof(a));
    a.arg[0] = (unsigned)u;
    a.arg[1] = 1;
    kuKernelCall((void *)fn_ctrl, &a);
    return u->Buttons;
}
int ark_get_brightness(void) {
    KuArgs a;
    int *u = (int *)((unsigned)g_kbuf | 0x40000000);
    memset(&a, 0, sizeof(a));
    u[0] = -1; u[1] = -1;
    a.arg[0] = (unsigned)&u[0];
    a.arg[1] = (unsigned)&u[1];
    kuKernelCall((void *)fn_get, &a);
    return u[0];
}
static void ark_set_brightness(int level) {
    KuArgs a;
    memset(&a, 0, sizeof(a));
    a.arg[0] = (unsigned)level;
    a.arg[1] = 0;
    kuKernelCall((void *)fn_set, &a);
}
void screen_blank(void) {
    int lvl = ark_get_brightness();
    g_bright = lvl >= 10 && lvl <= 100 ? lvl : 60;         /* what to bring back */
    { char m[60]; snprintf(m, sizeof(m), "screen blanked (was %d, will restore %d)", lvl, g_bright); logf_("%s", m); crumb(m); }
    ark_set_brightness(0);
    g_dark_level = ark_get_brightness();                   /* what the dark screen reads back, to notice when the system raises it */
    g_blanked = 1;
}
void screen_restore(void) {
    if (g_blanked) { char m[40]; snprintf(m, sizeof(m), "screen restored to %d", g_bright); logf_("%s", m); crumb(m); ark_set_brightness(g_bright); }
    g_blanked = 0;
}
#else
volatile int g_ark_ok;
int g_dark_level;
int ark_get_brightness(void) { return 0; }
void screen_blank(void) {}
void screen_restore(void) { g_blanked = 0; }
#endif

int exit_callback(int a, int b, void *c) { screen_restore(); sceKernelExitGame(); return 0; }

int power_callback(int unknown, int pwrflags, void *common) {
    if (pwrflags & PSP_POWER_CB_RESUME_COMPLETE) { g_resumed = 1; screen_restore(); }      /* woke from sleep: Wi-Fi must be rejoined */
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
