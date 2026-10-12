#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <linux/limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char  **items;
    size_t  count;
    size_t  capacity;
} str_list;

const size_t INITIAL_CAPACITY = 16;
int str_list_init(str_list *list)
{
    if (!list) return -1;

    list->items = malloc(INITIAL_CAPACITY * sizeof(char *));
    if (!list->items) return -1;

    list->count    = 0;
    list->capacity = INITIAL_CAPACITY;

    return 0;
}

int str_list_add(str_list *list, const char *str)
{
    if (!list || !str) return -1;

    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : INITIAL_CAPACITY;
        char **tmp = realloc(list->items, new_cap * sizeof(char *));
        if (!tmp) return -1;

        list->items    = tmp;
        list->capacity = new_cap;
    }

    char *copy = strdup(str);
    if (!copy) return -1;

    list->items[list->count++] = copy;
    return 0;
}

void str_list_free(str_list *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i)
        free(list->items[i]);
    free(list->items);
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void str_list_print(const str_list *list, FILE *out)
{
    if (!list || !out)
        return;
    for (size_t i = 0; i < list->count; ++i)
        fprintf(out, "%s\n", list->items[i]);
}

int str_list_from_lines(str_list *list, const char *src)
{
    if (!list || !src)
        return -1;

    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);

        if (len > 0) {
            char *line = malloc(len + 1);
            if (!line)
                return -1;
            memcpy(line, p, len);
            line[len] = '\0';

            if (str_list_add(list, line) != 0) {
                free(line);
                return -1;
            }
            free(line);
        }

        if (!nl)
            break;
        p = nl + 1;
    }

    return 0;
}

int str_list_contains(const str_list *list, const char *str)
{
    if (!list || !str)
        return 0;

    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i], str) == 0)
            return 1;
    }
    return 0;
}

typedef long (*str_int_key_fn)(const char *item);

typedef struct {
    long  key;
    char *str;
} keyed_item;

static int keyed_cmp_desc(const void *a, const void *b)
{
    const keyed_item *ka = (const keyed_item *)a;
    const keyed_item *kb = (const keyed_item *)b;
    if (ka->key > kb->key) return -1;
    if (ka->key < kb->key) return  1;
    return 0;
}

void str_list_sort_by_key_desc_annotated(str_list *list, str_int_key_fn key_fn)
{
    if (!list || !key_fn || list->count == 0)
        return;

    keyed_item *arr = malloc(list->count * sizeof(*arr));
    if (!arr)
        return;

    for (size_t i = 0; i < list->count; ++i) {
        arr[i].key = key_fn(list->items[i]);
        arr[i].str = list->items[i];
    }

    qsort(arr, list->count, sizeof(*arr), keyed_cmp_desc);

    for (size_t i = 0; i < list->count; ++i) {
        const char *old     = arr[i].str;
        size_t      old_len = strlen(old);

        char suffix[32];
        int  suffix_len = snprintf(suffix, sizeof(suffix),
                                   "  [deps: %ld]", arr[i].key);
        if (suffix_len < 0)
            suffix_len = 0;

        char *new_str = malloc(old_len + (size_t)suffix_len + 1);
        if (!new_str) {
            list->items[i] = arr[i].str;
            continue;
        }

        memcpy(new_str, old, old_len);
        memcpy(new_str + old_len, suffix, (size_t)suffix_len + 1);

        free(arr[i].str);
        list->items[i] = new_str;
    }

    free(arr);
}

char *modify_path_in_cmd(const char *cmd_template, const char *path) {
  int len = snprintf(NULL, 0, cmd_template, path);
  if (len < 0)
    return NULL;

  char *result = malloc(len + 1);
  if (result == NULL)
    return NULL;

  snprintf(result, len + 1, cmd_template, path);

  return result;
}

char *read_all_from_stream(FILE *fp) {
  if (!fp)
    return NULL;

  const size_t CHUNK_SIZE = 1024;

  size_t capacity = CHUNK_SIZE;
  size_t total_read = 0;

  char *buffer = malloc(capacity);
  if (!buffer)
    return NULL;

  size_t bytes_read;
  while ((bytes_read = fread(buffer + total_read, 1, CHUNK_SIZE, fp)) > 0) {
    total_read += bytes_read;

    if (total_read + CHUNK_SIZE + 1 > capacity) {
      capacity += CHUNK_SIZE;
      char *new_buf = realloc(buffer, capacity);
      if (!new_buf) {
        free(buffer);
        return NULL;
      }
      buffer = new_buf;
    }
  }

  buffer[total_read] = '\0';

  return buffer;
}

static int run_elf_cmd(const char *cmd_name,
                       const char *cmd_template,
                       char **out)
{
  if (cmd_name == NULL || cmd_template == NULL || out == NULL) {
    return -1;
  }
  if (strchr(cmd_name, '\'')) {
    return -1;
  }

  char *cmd = modify_path_in_cmd(cmd_template, cmd_name);
  if (cmd == NULL) {
    return -1;
  }

  FILE *fp = popen(cmd, "r");
  if (fp == NULL) {
    free(cmd);
    return -1;
  }

  char *res = read_all_from_stream(fp);

  pclose(fp);
  free(cmd);

  if (res == NULL) {
    return -1;
  }

  *out = res;
  return 0;
}

int get_elf_deps(const char *cmd_name, char **deps) {
  const char *tpl =
      "readelf -d '%s' | grep \"(NEEDED)\" | grep -Po '(?<=\\[)[^\\]]+(?=\\])'";
  return run_elf_cmd(cmd_name, tpl, deps);
}

int get_elf_arch(const char *cmd_name, char **arch) {
  const char *tpl =
      "readelf -h '%s' | grep \"Machine:\" | grep -Po '(?<=Machine:)\\s+\\K\\S.*'";
  return run_elf_cmd(cmd_name, tpl, arch);
}

int get_elf_type(const char *cmd_name, char **type) {
  const char *tpl =
      "readelf -h '%s' | grep \"Type:\" | grep -Po '(?<=Type:)\\s+\\K\\w+'";
  return run_elf_cmd(cmd_name, tpl, type);
}

int get_elf_kind_interp(const char *cmd_name, char **result)
{
    const char *tpl =
        "readelf -l '%s' | grep -q INTERP && echo executable || echo shared";
    return run_elf_cmd(cmd_name, tpl, result);
}

int get_elf_kind_linkage(const char *cmd_name, char **result)
{
    const char *tpl =
        "readelf -l '%s' | grep -q DYNAMIC && echo dynamic || echo static";
    return run_elf_cmd(cmd_name, tpl, result);
}

char *get_elf_name(const char *path)
{
    if (path == NULL)
        return NULL;

    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;

    if (*base == '\0')
        return NULL;

    return strdup(base);
}

static long count_needed_deps(const char *path)
{
    char *deps_raw = NULL;
    if (get_elf_deps(path, &deps_raw) != 0 || !deps_raw)
        return 0;

    str_list deps;
    if (str_list_init(&deps) != 0) {
        free(deps_raw);
        return 0;
    }
    str_list_from_lines(&deps, deps_raw);
    free(deps_raw);

    long n = (long)deps.count;
    str_list_free(&deps);
    return n;
}

bool is_elf_file(const char *filepath) {
  FILE *file = fopen(filepath, "rb");
  if (!file) {
    return 0;
  }

  unsigned char magic[4];
  size_t read_bytes = fread(magic, 1, 4, file);
  fclose(file);

  if (read_bytes < 4) {
    return 0;
  }

  return (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' &&
          magic[3] == 'F');
}


typedef bool (*elf_predicate_t)(const char *fullpath, void *user_data);

size_t find_elfs_in_dir(const char *path,
                        elf_predicate_t pred,
                        void *user_data,
                        str_list *res) {
  DIR *dir = opendir(path);
  if (!dir)
    return 0;
  size_t proc_dirs = 1;

  struct dirent *entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char fullpath[PATH_MAX];
    snprintf(fullpath, sizeof(fullpath), "%s%s", path, entry->d_name);

    struct stat statbuf;
    if (lstat(fullpath, &statbuf) == -1) {
      continue;
    }

    if (S_ISDIR(statbuf.st_mode)) {
      proc_dirs += find_elfs_in_dir(fullpath, pred, user_data, res);
    } else if (S_ISREG(statbuf.st_mode)) {
      if (is_elf_file(fullpath) && (!pred || pred(fullpath, user_data))) {
        str_list_add(res, fullpath);
      }
    }
  }

  closedir(dir);
  return proc_dirs;
}

typedef struct {
    const char *lib_name;
    const char *arch;
} find_ctx_t;

static void rstrip_nl(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r'))
        s[--n] = '\0';
}

bool depends_on_lib(const char *fullpath, void *user_data) {
    find_ctx_t *ctx = (find_ctx_t *)user_data;

    char *kind = NULL;
    if (get_elf_kind_interp(fullpath, &kind) != 0 || !kind)
        return false;
    rstrip_nl(kind);
    int is_exe = (strcmp(kind, "executable") == 0);
    free(kind);
    if (!is_exe)
        return false;

    if (ctx->arch) {
        char *arch = NULL;
        if (get_elf_arch(fullpath, &arch) != 0 || !arch)
            return false;
        rstrip_nl(arch);
        int same = (strcmp(arch, ctx->arch) == 0);
        free(arch);
        if (!same)
            return false;
    }

    char *deps_raw = NULL;
    if (get_elf_deps(fullpath, &deps_raw) != 0 || !deps_raw)
        return false;

    str_list deps;
    if (str_list_init(&deps) != 0) {
        free(deps_raw);
        return false;
    }
    str_list_from_lines(&deps, deps_raw);
    free(deps_raw);

    int found = str_list_contains(&deps, ctx->lib_name);
    str_list_free(&deps);
    return found;
}

typedef struct {
    const char *arch;
} static_ctx_t;

bool is_static_elf(const char *fullpath, void *user_data) {
    static_ctx_t *ctx = (static_ctx_t *)user_data;

    char *type = NULL;
    if (get_elf_type(fullpath, &type) != 0 || !type)
        return false;
    rstrip_nl(type);
    int is_exec = (strcmp(type, "EXEC") == 0);
    free(type);
    if (!is_exec)
        return false;

    char *link = NULL;
    if (get_elf_kind_linkage(fullpath, &link) != 0 || !link)
        return false;
    rstrip_nl(link);
    int is_static = (strcmp(link, "static") == 0);
    free(link);
    if (!is_static)
        return false;

    if (ctx && ctx->arch) {
        char *arch = NULL;
        if (get_elf_arch(fullpath, &arch) != 0 || !arch)
            return false;
        rstrip_nl(arch);
        int same = (strcmp(arch, ctx->arch) == 0);
        free(arch);
        if (!same)
            return false;
    }

    return true;
}

typedef struct {
    const char *lib_name;
    const char *arch;
} other_arch_ctx_t;

bool depends_on_lib_other_arch(const char *fullpath, void *user_data) {
    other_arch_ctx_t *ctx = (other_arch_ctx_t *)user_data;

    char *kind = NULL;
    if (get_elf_kind_interp(fullpath, &kind) != 0 || !kind)
        return false;
    rstrip_nl(kind);
    int is_exe = (strcmp(kind, "executable") == 0);
    free(kind);
    if (!is_exe)
        return false;

    char *arch = NULL;
    if (get_elf_arch(fullpath, &arch) != 0 || !arch)
        return false;
    rstrip_nl(arch);
    int different = (strcmp(arch, ctx->arch) != 0);
    free(arch);
    if (!different)
        return false;

    char *deps_raw = NULL;
    if (get_elf_deps(fullpath, &deps_raw) != 0 || !deps_raw)
        return false;

    str_list deps;
    if (str_list_init(&deps) != 0) {
        free(deps_raw);
        return false;
    }
    str_list_from_lines(&deps, deps_raw);
    free(deps_raw);

    int found = str_list_contains(&deps, ctx->lib_name);
    str_list_free(&deps);

    return found;
}

void str_list_annotate_with_arch_and_deps(str_list *list)
{
    if (!list || list->count == 0)
        return;

    /* ключ = deps, для сортировки; arch читаем тут же */
    typedef struct { long key; char *str; } item_t;
    item_t *arr = malloc(list->count * sizeof(*arr));
    if (!arr) return;

    for (size_t i = 0; i < list->count; ++i) {
        arr[i].key = count_needed_deps(list->items[i]);
        arr[i].str = list->items[i];
    }
    qsort(arr, list->count, sizeof(*arr), keyed_cmp_desc);

    for (size_t i = 0; i < list->count; ++i) {
        char *arch = NULL;
        get_elf_arch(arr[i].str, &arch);
        if (arch) rstrip_nl(arch);

        char suffix[128];
        int n = snprintf(suffix, sizeof(suffix),
                         "  [arch: %s, deps: %ld]",
                         arch ? arch : "?", arr[i].key);
        if (n < 0) n = 0;

        size_t old_len = strlen(arr[i].str);
        char *new_str = malloc(old_len + (size_t)n + 1);
        if (!new_str) {
            list->items[i] = arr[i].str;
            free(arch);
            continue;
        }
        memcpy(new_str, arr[i].str, old_len);
        memcpy(new_str + old_len, suffix, (size_t)n + 1);

        free(arr[i].str);
        free(arch);
        list->items[i] = new_str;
    }
    free(arr);
}

typedef struct {
    FILE *out;
    FILE *file;
} report_t;

void report_printf_file_only(report_t *r, const char *fmt, ...)
{
    if (!r || !r->file)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(r->file, fmt, ap);
    va_end(ap);
}

static void report_printf(report_t *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(r->out, fmt, ap);
    va_end(ap);

    if (r->file) {
        va_start(ap, fmt);
        vfprintf(r->file, fmt, ap);
        va_end(ap);
    }
}

static void report_str_list(report_t *r, const str_list *list) {
    for (size_t i = 0; i < list->count; ++i)
        report_printf(r, "%s\n", list->items[i]);
}

typedef struct {
    const char *lib_path;
    const char *dir_path;
    const char *report_path;
    int do_static;
    int do_other_arch;
} options_t;

static void print_usage(FILE *out, const char *prog) {
    fprintf(out,
        "Usage: %s [options] <library> <directory>\n"
        "\n"
        "Recursively scan <directory> and report ELF executables that\n"
        "depend on the shared library <library>.\n"
        "\n"
        "Options:\n"
        "  -h            Show this help and exit\n"
        "  -s            Also list statically linked executables\n"
        "  -o            Also list executables of a different architecture\n"
        "                that still depend on <library>\n"
        "  -r <file>     Additionally write the report to <file>\n"
        "\n"
        "Arguments:\n"
        "  <library>     Path to a shared library (.so)\n"
        "  <directory>   Directory to search recursively\n"
        "\n"
        "Examples:\n"
        "  # Find programs in /usr/bin that use libc\n"
        "  %s /lib/x86_64-linux-gnu/libc.so.6 /usr/bin\n"
        "\n"
        "  # Same, plus statically linked binaries, save report to report.txt\n"
        "  %s -s -r report.txt /lib/x86_64-linux-gnu/libz.so.1 /usr/bin\n"
        "\n"
        "  # Also include consumers built for another architecture\n"
        "  %s -s -o /lib/x86_64-linux-gnu/libssl.so.3 /usr/bin\n",
        prog, prog, prog, prog);
}

static int parse_options(int argc, char *argv[], options_t *opt) {
    opt->lib_path    = NULL;
    opt->dir_path    = NULL;
    opt->report_path = NULL;
    opt->do_static   = 0;
    opt->do_other_arch = 0;

    int c;
    while ((c = getopt(argc, argv, "hsor:")) != -1) {
        switch (c) {
            case 'h':
                print_usage(stdout, argv[0]);
                exit(EXIT_SUCCESS);
            case 's':
                opt->do_static = 1;
                break;
            case 'o':
                opt->do_other_arch = 1;
                break;
            case 'r':
                opt->report_path = optarg;
                break;
            default:
                print_usage(stderr, argv[0]);
                return -1;
        }
    }

    if (argc - optind != 2) {
        fprintf(stderr, "%s: expected <library> and <directory>\n", argv[0]);
        print_usage(stderr, argv[0]);
        return -1;
    }

    opt->lib_path = argv[optind];
    opt->dir_path = argv[optind + 1];
    return 0;
}

static int run_search(const options_t *opt, report_t *rep,
                      const char *lib_name, const char *arch) {
    str_list res;
    if (str_list_init(&res) != 0) {
        fprintf(stderr, "error: out of memory\n");
        return -1;
    }

    find_ctx_t ctx = { .lib_name = lib_name, .arch = arch };
    size_t pr_dirs = find_elfs_in_dir(opt->dir_path, depends_on_lib, &ctx, &res);

    str_list_sort_by_key_desc_annotated(&res, count_needed_deps);

    report_printf(rep, "\nmatched elfs:\n");
    report_str_list(rep, &res);
    report_printf(rep, "looked dirs: %zu\n", pr_dirs);
    str_list_free(&res);

    if (opt->do_static) {
        str_list statics;
        if (str_list_init(&statics) != 0) {
            fprintf(stderr, "error: out of memory\n");
            return -1;
        }

        static_ctx_t sctx = { .arch = arch };
        size_t sd = find_elfs_in_dir(opt->dir_path, is_static_elf, &sctx, &statics);

        report_printf(rep, "\nstatic executables:\n");
	str_list_annotate_with_arch_and_deps(&statics);
        report_str_list(rep, &statics);
        report_printf(rep, "looked dirs: %zu\n", sd);
        str_list_free(&statics);
    }

    if (opt->do_other_arch) {
        str_list others;
        if (str_list_init(&others) != 0) {
            fprintf(stderr, "error: out of memory\n");
            return -1;
        }

        other_arch_ctx_t octx = { .lib_name = lib_name, .arch = arch };
        size_t od = find_elfs_in_dir(opt->dir_path, depends_on_lib_other_arch,
                                     &octx, &others);

        report_printf(rep, "\ndepends on %s but different arch:\n", lib_name);
	str_list_annotate_with_arch_and_deps(&others);
        report_str_list(rep, &others);
        report_printf(rep, "looked dirs: %zu\n", od);
        str_list_free(&others);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    options_t opt;
    if (parse_options(argc, argv, &opt) != 0)
        return EXIT_FAILURE;

    if (!is_elf_file(opt.lib_path)) {
        fprintf(stderr, "%s: '%s' is not an ELF file\n",
                argv[0], opt.lib_path);
        return EXIT_FAILURE;
    }

    char *type = NULL;
    if (get_elf_kind_interp(opt.lib_path, &type) != 0 || !type) {
        fprintf(stderr, "%s: failed to inspect '%s'\n",
                argv[0], opt.lib_path);
        return EXIT_FAILURE;
    }
    rstrip_nl(type);
    int is_shared = (strcmp(type, "shared") == 0);
    free(type);
    if (!is_shared) {
        fprintf(stderr, "%s: '%s' is not a shared library\n",
                argv[0], opt.lib_path);
        return EXIT_FAILURE;
    }

    char *lib_name = get_elf_name(opt.lib_path);
    if (!lib_name) {
        fprintf(stderr, "%s: failed to derive library name from '%s'\n",
                argv[0], opt.lib_path);
        return EXIT_FAILURE;
    }

    char *arch = NULL;
    if (get_elf_arch(opt.lib_path, &arch) != 0 || !arch) {
        fprintf(stderr, "%s: failed to read architecture of '%s'\n",
                argv[0], opt.lib_path);
        free(lib_name);
        return EXIT_FAILURE;
    }
    rstrip_nl(arch);

    report_t rep = { .out = stdout, .file = NULL };
    if (opt.report_path) {
        rep.file = fopen(opt.report_path, "w");
        if (!rep.file) {
            fprintf(stderr, "%s: cannot open report file '%s'\n",
                    argv[0], opt.report_path);
            free(arch);
            free(lib_name);
            return EXIT_FAILURE;
        }
    }

    if (rep.file) {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char datebuf[64];
        if (tm_info && strftime(datebuf, sizeof(datebuf),
                                "%Y-%m-%d %H:%M:%S", tm_info) > 0) {
            report_printf_file_only(&rep, "date: %s\n", datebuf);
        }
    }

    report_printf(&rep, "lib: %s\n", lib_name);
    report_printf(&rep, "arch: %s\n", arch);
    report_printf(&rep, "search: %s\n", opt.dir_path);

    int rc = run_search(&opt, &rep, lib_name, arch);

    if (rep.file)
        fclose(rep.file);
    free(arch);
    free(lib_name);

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
