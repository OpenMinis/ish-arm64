#include <string.h>
#include "kernel/calls.h"

#define PRCTL_GET_DUMPABLE_ 3
#define PRCTL_SET_DUMPABLE_ 4
#define PRCTL_SET_KEEPCAPS_ 8
#define PRCTL_SET_NAME_ 15

int_t sys_prctl(dword_t option, addr_t arg2, addr_t UNUSED(arg3), addr_t UNUSED(arg4), addr_t UNUSED(arg5)) {
    switch (option) {
        // Nothing dumps core here, but sftp-server, sshd and other hardened
        // programs set this at startup and exit when it fails.
        case PRCTL_GET_DUMPABLE_:
            STRACE("prctl(PR_GET_DUMPABLE)");
            return current->undumpable ? 0 : 1;
        case PRCTL_SET_DUMPABLE_:
            STRACE("prctl(PR_SET_DUMPABLE, %d)", arg2);
            if (arg2 > 1)
                return _EINVAL;
            current->undumpable = arg2 == 0;
            return 0;
        case PRCTL_SET_KEEPCAPS_:
            // stub
            return 0;
        case PRCTL_SET_NAME_: {
            char name[16];
            if (user_read_string(arg2, name, sizeof(name) - 1))
                return _EFAULT;
            name[sizeof(name) - 1] = '\0';
            STRACE("prctl(PRCTL_SET_NAME, \"%s\")", name);
            strcpy(current->comm, name);
            return 0;
        }
        default:
            STRACE("prctl(%#x)", option);
            return _EINVAL;
    }
}

#define ARCH_SET_GS_ 0x1001
#define ARCH_SET_FS_ 0x1002
#define ARCH_GET_FS_ 0x1003
#define ARCH_GET_GS_ 0x1004

int_t sys_arch_prctl(int_t code, addr_t addr) {
    STRACE("arch_prctl(%#x, %#llx)", code, (unsigned long long) addr);
#ifdef GUEST_X86_64
    switch (code) {
        case ARCH_SET_FS_:
            current->cpu.tls_ptr = addr;
            return 0;
        case ARCH_SET_GS_:
            current->cpu.gs_base = addr;
            return 0;
        case ARCH_GET_FS_:
            return user_put(addr, current->cpu.tls_ptr) ? _EFAULT : 0;
        case ARCH_GET_GS_:
            return user_put(addr, current->cpu.gs_base) ? _EFAULT : 0;
    }
#endif
    return _EINVAL;
}

#define REBOOT_MAGIC1 0xfee1dead
#define REBOOT_MAGIC2 672274793
#define REBOOT_MAGIC2A 85072278
#define REBOOT_MAGIC2B 369367448
#define REBOOT_MAGIC2C 537993216

#define REBOOT_CMD_CAD_OFF 0
#define REBOOT_CMD_CAD_ON 0x89abcdef

int_t sys_reboot(int_t magic, int_t magic2, int_t cmd) {
    STRACE("reboot(%#x, %d, %d)", magic, magic2, cmd);
    if (!superuser())
        return _EPERM;
    if (magic != (int) REBOOT_MAGIC1 ||
            (magic2 != REBOOT_MAGIC2 &&
             magic2 != REBOOT_MAGIC2A &&
             magic2 != REBOOT_MAGIC2B &&
             magic2 != REBOOT_MAGIC2C))
        return _EINVAL;

    switch (cmd) {
        case REBOOT_CMD_CAD_ON:
        case REBOOT_CMD_CAD_OFF:
            return 0;
        default:
            return _EPERM;
    }
}
