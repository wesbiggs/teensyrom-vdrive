/* Built with -DVD_DEV0=10 -DVD_DEV1=11 to prove the compile-time device numbers take effect. */
#include "../d64/vdrive.h"
#include <stdio.h>
#include <stdlib.h>
int main(void)
{
    static vdrive_t v;
    vdrive_init(&v);
    int ok = vdrive_unit_for(&v, 10) == 0 && vdrive_unit_for(&v, 11) == 1 &&
             vdrive_unit_for(&v, 8) == -1 && vdrive_unit_for(&v, 9) == -1;
    printf("%s compile-time VD_DEV0/VD_DEV1 = %d/%d\n", ok ? "PASS" : "FAIL", v.dev[0], v.dev[1]);
    return ok ? 0 : 1;
}
