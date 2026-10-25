#include "sprfst/natives.h"

const NativeFn SPRFST_NATIVES[NF_COUNT] = {
#define X(id, m, n, s, d) { m, n, s, d, NF_##id },
    SPRFST_NATIVE_LIST(X)
#undef X
};

const char **natives_module_names(int *count) {
    static const char *mods[64];
    static int n = -1;
    if (n < 0) {
        n = 0;
        for (int i = 0; i < NF_COUNT; i++) {
            const char *m = SPRFST_NATIVES[i].module;
            if (!m || !*m || m[0] == '@') continue;
            bool found = false;
            for (int k = 0; k < n; k++) if (strcmp(mods[k], m) == 0) { found = true; break; }
            if (!found && n < 63) mods[n++] = m;
        }
    }
    *count = n;
    return mods;
}

int natives_lookup(const char *module, const char *name) {
    for (int i = 0; i < NF_COUNT; i++) {
        const NativeFn *f = &SPRFST_NATIVES[i];
        if (strcmp(f->name, name) != 0) continue;
        if (!module) { if (!f->module[0]) return i; continue; }
        if (strcmp(f->module, module) == 0) return i;
    }
    return -1;
}
