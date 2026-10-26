// ReSys12 kernel helper, user side (vita/kmod/rs12_kctrl.c)
#ifndef RS12_KCTRL_H
#define RS12_KCTRL_H
unsigned int rs12KCtrlPsState(void); // bits 0-14 single PS presses, 15-29 double presses, 30 held
int rs12KCtrlGoHome(void);           // press PS for the system once (after the app lets go of its lock)
int rs12KCtrlInstallOnExit(void);    // once the app has exited: ux0:data/resys12/update/files -> the app folder
#include <stdint.h>
// measurements only: sample the PC / LR of a thread of this app (user_thid -1 stops), read the samples
int rs12KCtrlProfStart(int user_thid, int cpu_mask);
int rs12KCtrlProfRead(uint32_t *dst, int max, int *err);
#endif
