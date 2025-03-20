#include <common/util.h>
#include <object/cap_group.h>

inline static bool is_system_services(struct cap_group *cg)
{
    // if end with .srv, it is a system service
    if (strstr(cg->cap_group_name, "srv") != NULL) {
        return true;
    }

    if (strncmp(cg->cap_group_name, "chcore_shell.bin", 18) == 0) {
        return true;
    }

    return false;
}
