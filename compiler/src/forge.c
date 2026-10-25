/* ========================================================================
   Forge — the SPRFST package manager

   Forge has no central registry and needs no network service.  A package
   is a directory with a project.sprfst and a src/ folder.  You add one by
   path or by git URL; Forge copies it into packages/, records the exact
   content digest in forge.lock, and verifies that digest on every install.

       sprfst add  maths --path=../maths
       sprfst add  colour --git=https://example.com/colour.git
       sprfst remove colour
       sprfst install
       sprfst package                  (build a .forge bundle to share)
   ======================================================================== */
#define _GNU_SOURCE 1
#include "sprfst/driver.h"
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

/* --------------------------------------------------------------- utils */
static void collect_files(const char *dir, const char *prefix, StrBuf *list) {
    int n = 0;
    char **names = list_dir(dir, &n, NULL);
    if (!names) return;
    /* stable order keeps digests reproducible */
    for (int i = 0; i < n; i++)
        for (int k = i + 1; k < n; k++)
            if (strcmp(names[i], names[k]) > 0) { char *t = names[i]; names[i] = names[k]; names[k] = t; }
    for (int i = 0; i < n; i++) {
        if (names[i][0] == '.') { free(names[i]); continue; }
        char full[1600], rel[1600];
        snprintf(full, sizeof full, "%s/%s", dir, names[i]);
        snprintf(rel, sizeof rel, "%s%s%s", prefix, *prefix ? "/" : "", names[i]);
        if (dir_exists(full)) {
            if (strcmp(names[i], "build") != 0 && strcmp(names[i], "packages") != 0)
                collect_files(full, rel, list);
        } else {
            sb_puts(list, rel);
            sb_putc(list, '\n');
        }
        free(names[i]);
    }
    free(names);
}

static char *digest_tree(const char *dir) {
    StrBuf list; sb_init(&list);
    collect_files(dir, "", &list);
    StrBuf all; sb_init(&all);
    const char *p = list.data ? list.data : "";
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char rel[1600];
        snprintf(rel, sizeof rel, "%.*s", (int)len, p);
        char full[1700];
        snprintf(full, sizeof full, "%.99s/%.1599s", dir, rel);
        size_t fn = 0;
        char *data = read_file(full, &fn);
        sb_puts(&all, rel);
        sb_putc(&all, '\n');
        if (data) { sb_put(&all, data, fn); free(data); }
        if (!eol) break;
        p = eol + 1;
    }
    char *hex = malloc(65);
    sha256_hex(all.data ? all.data : "", all.len, hex);
    sb_free(&all);
    sb_free(&list);
    return hex;
}

static bool copy_tree(const char *from, const char *to) {
    if (!dir_exists(from)) return false;
    make_dir_all(to);
    int n = 0;
    char **names = list_dir(from, &n, NULL);
    if (!names) return false;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        if (names[i][0] == '.') { free(names[i]); continue; }
        char src[1600], dst[1600];
        snprintf(src, sizeof src, "%s/%s", from, names[i]);
        snprintf(dst, sizeof dst, "%s/%s", to, names[i]);
        if (dir_exists(src)) {
            if (strcmp(names[i], "build") && strcmp(names[i], "packages")) ok &= copy_tree(src, dst);
        } else {
            size_t len = 0;
            char *data = read_file(src, &len);
            if (data) { ok &= write_file_bytes(dst, data, len); free(data); }
        }
        free(names[i]);
    }
    free(names);
    return ok;
}

/* --------------------------------------------------------- project file */
static bool project_set_dep(const char *root, const char *name, const char *spec, bool remove) {
    char path[1300];
    snprintf(path, sizeof path, "%s/project.sprfst", root);
    size_t n = 0;
    char *text = read_file(path, &n);
    if (!text) return false;

    StrBuf out; sb_init(&out);
    bool in_pkgs = false, written = false;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char trimmed[1024];
        snprintf(trimmed, sizeof trimmed, "%s", line);
        char *t = trimmed;
        while (*t == ' ' || *t == '\t') t++;

        if (*t == '[') {
            if (in_pkgs && !written && !remove) { sb_printf(&out, "%s = %s\n", name, spec); written = true; }
            in_pkgs = strncmp(t, "[packages]", 10) == 0;
            sb_printf(&out, "%s\n", line);
            continue;
        }
        if (in_pkgs) {
            char key[256] = { 0 };
            sscanf(t, "%255[^= \t]", key);
            if (strcmp(key, name) == 0) {
                if (remove) continue;
                sb_printf(&out, "%s = %s\n", name, spec);
                written = true;
                continue;
            }
        }
        sb_printf(&out, "%s\n", line);
    }
    if (!written && !remove) {
        if (!in_pkgs) sb_puts(&out, "\n[packages]\n");
        sb_printf(&out, "%s = %s\n", name, spec);
    }
    bool ok = write_file_bytes(path, out.data ? out.data : "", out.len);
    sb_free(&out);
    free(text);
    return ok;
}

/* ------------------------------------------------------------- lockfile */
static void lock_write(const char *root, const char *name, const char *source, const char *digest) {
    char path[1300];
    snprintf(path, sizeof path, "%s/forge.lock", root);
    size_t n = 0;
    char *old = read_file(path, &n);
    StrBuf out; sb_init(&out);
    sb_printf(&out, "# Forge lockfile — do not edit by hand\n");
    bool replaced = false;
    if (old) {
        char *save = NULL;
        for (char *line = strtok_r(old, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (line[0] == '#' || !line[0]) continue;
            char key[256] = { 0 };
            sscanf(line, "%255s", key);
            if (strcmp(key, name) == 0) {
                if (digest) { sb_printf(&out, "%s %s %s\n", name, source, digest); replaced = true; }
                else replaced = true;          /* removal */
                continue;
            }
            sb_printf(&out, "%s\n", line);
        }
        free(old);
    }
    if (!replaced && digest) sb_printf(&out, "%s %s %s\n", name, source, digest);
    write_file_bytes(path, out.data ? out.data : "", out.len);
    sb_free(&out);
}

static char *lock_read(Arena *a, const char *root, const char *name, char **source_out) {
    char path[1300];
    snprintf(path, sizeof path, "%s/forge.lock", root);
    size_t n = 0;
    char *text = read_file(path, &n);
    if (!text) return NULL;
    char *result = NULL;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char key[256] = { 0 }, src[1024] = { 0 }, dig[128] = { 0 };
        if (sscanf(line, "%255s %1023s %127s", key, src, dig) != 3) continue;
        if (strcmp(key, name) != 0) continue;
        result = arena_strdup(a, dig);
        if (source_out) *source_out = arena_strdup(a, src);
        break;
    }
    free(text);
    return result;
}

/* ------------------------------------------------------------- install */
static bool fetch_git(const char *url, const char *dest) {
    char cmd[2600];
    snprintf(cmd, sizeof cmd, "git clone --depth 1 --quiet '%s' '%s' 2>/dev/null", url, dest);
    int rc = system(cmd);
    if (rc != 0) return false;
    char gitdir[1600];
    snprintf(gitdir, sizeof gitdir, "%s/.git", dest);
    remove_dir_all(gitdir);
    return true;
}

static bool install_one(const char *root, const char *name, const char *spec, bool verify) {
    char dest[1400];
    snprintf(dest, sizeof dest, "%s/packages/%s", root, name);

    bool ok = false;
    if (strncmp(spec, "path:", 5) == 0) {
        char from[1300];
        snprintf(from, sizeof from, "%s", spec + 5);
        char abs[1600];
        if (from[0] != '/') snprintf(abs, sizeof abs, "%s/%s", root, from);
        else snprintf(abs, sizeof abs, "%s", from);
        if (!dir_exists(abs)) {
            fprintf(stderr, "  %serror%s  `%s` is not a directory\n", C_RED, C_RESET, abs);
            return false;
        }
        remove_dir_all(dest);
        ok = copy_tree(abs, dest);
    } else if (strncmp(spec, "git:", 4) == 0) {
        remove_dir_all(dest);
        ok = fetch_git(spec + 4, dest);
        if (!ok) fprintf(stderr, "  %serror%s  could not clone %s\n", C_RED, C_RESET, spec + 4);
    } else {
        fprintf(stderr, "  %serror%s  `%s` has no source — use --path= or --git=\n", C_RED, C_RESET, name);
        return false;
    }
    if (!ok) return false;

    char *digest = digest_tree(dest);
    if (verify) {
        Arena a;
        arena_init(&a);
        char *expect = lock_read(&a, root, name, NULL);
        if (expect && strcmp(expect, digest) != 0) {
            fprintf(stderr, "  %srefused%s  %s does not match forge.lock\n", C_RED, C_RESET, name);
            fprintf(stderr, "    %sexpected %s%s\n    %sgot      %s%s\n", C_DIM, expect, C_RESET, C_DIM, digest, C_RESET);
            arena_free(&a);
            free(digest);
            return false;
        }
        arena_free(&a);
    }
    lock_write(root, name, spec, digest);
    fprintf(stderr, "  %sinstalled%s %-16s %s%.12s%s\n", C_GREEN, C_RESET, name, C_DIM, digest, C_RESET);
    free(digest);
    return true;
}

/* -------------------------------------------------------------- bundle */
static int make_bundle(const char *root) {
    Project p = project_load(root);
    StrBuf list; sb_init(&list);
    collect_files(root, "", &list);

    StrBuf out; sb_init(&out);
    sb_printf(&out, "FORGE1\n%s\n%s\n", p.name, p.version);

    int count = 0;
    const char *q = list.data ? list.data : "";
    StrBuf body; sb_init(&body);
    while (*q) {
        const char *eol = strchr(q, '\n');
        size_t len = eol ? (size_t)(eol - q) : strlen(q);
        char rel[1600];
        snprintf(rel, sizeof rel, "%.*s", (int)len, q);
        if (strcmp(rel, "forge.lock") != 0 && strncmp(rel, "build/", 6) != 0) {
            char full[1700];
            snprintf(full, sizeof full, "%.99s/%.1599s", root, rel);
            size_t fn = 0;
            char *data = read_file(full, &fn);
            if (data) {
                sb_printf(&body, "file %s %zu\n", rel, fn);
                sb_put(&body, data, fn);
                sb_putc(&body, '\n');
                free(data);
                count++;
            }
        }
        if (!eol) break;
        q = eol + 1;
    }
    char digest[65];
    sha256_hex(body.data ? body.data : "", body.len, digest);
    sb_printf(&out, "files %d\nsha256 %s\n", count, digest);
    sb_put(&out, body.data ? body.data : "", body.len);

    char path[1400];
    snprintf(path, sizeof path, "%s/%s-%s.forge", root, p.name, p.version);
    write_file_bytes(path, out.data ? out.data : "", out.len);
    fprintf(stderr, "  %spackaged%s %s  %s%d files, %zu KB, %.12s%s\n", C_GREEN, C_RESET, path,
            C_DIM, count, out.len / 1024, digest, C_RESET);
    sb_free(&out);
    sb_free(&body);
    sb_free(&list);
    return 0;
}

/* ---------------------------------------------------------------- entry */
int cmd_forge(const char *verb, int argc, char **argv) {
    Arena a;
    arena_init(&a);
    char *rootp = project_find_root(&a, ".");
    if (!rootp) {
        fprintf(stderr, "  %serror%s  no project.sprfst here — run  sprfst new <name>  first\n", C_RED, C_RESET);
        arena_free(&a);
        return 1;
    }
    char root[1200];
    snprintf(root, sizeof root, "%s", rootp);

    const char *name = NULL;
    char spec[1200] = "";
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "--path=", 7) == 0) snprintf(spec, sizeof spec, "path:%s", argv[i] + 7);
        else if (strncmp(argv[i], "--git=", 6) == 0) snprintf(spec, sizeof spec, "git:%s", argv[i] + 6);
        else if (argv[i][0] != '-') name = argv[i];
    }

    int rc = 0;
    if (strcmp(verb, "add") == 0) {
        if (!name) { fprintf(stderr, "  usage: sprfst add <name> --path=<dir> | --git=<url>\n"); rc = 1; }
        else {
            if (!spec[0]) snprintf(spec, sizeof spec, "path:packages/%s", name);
            if (install_one(root, name, spec, false)) project_set_dep(root, name, spec, false);
            else rc = 1;
        }
    } else if (strcmp(verb, "remove") == 0) {
        if (!name) { fprintf(stderr, "  usage: sprfst remove <name>\n"); rc = 1; }
        else {
            char dest[1400];
            snprintf(dest, sizeof dest, "%s/packages/%s", root, name);
            remove_dir_all(dest);
            project_set_dep(root, name, "", true);
            lock_write(root, name, "", NULL);
            fprintf(stderr, "  %sremoved%s %s\n", C_GREEN, C_RESET, name);
        }
    } else if (strcmp(verb, "install") == 0) {
        Project p = project_load(root);
        if (!p.ndeps) fprintf(stderr, "  %snothing to install%s\n", C_DIM, C_RESET);
        for (int i = 0; i < p.ndeps; i++) {
            char dep[200];
            snprintf(dep, sizeof dep, "%s", p.deps[i]);
            char *eq = strchr(dep, '=');
            if (!eq) continue;
            *eq = 0;
            if (!install_one(root, dep, eq + 1, true)) rc = 1;
        }
    } else if (strcmp(verb, "package") == 0) {
        rc = make_bundle(root);
    }
    arena_free(&a);
    return rc;
}
