//
// Created by YinSheng Tang on 2021/7/21.
//

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <sys/auxv.h>
#include "sd_log.h"
#include "semi_dlfcn.h"

#define LOG_TAG "semi_dlfcn"

#define LIKELY(cond) (__builtin_expect(!!(cond), 1))
#define UNLIKELY(cond) (__builtin_expect(!!(cond), 0))

#define SEMI_DLINFO_MAGIC 0xFE5D15D1

typedef struct {
    uint32_t magic;

    const char* pathname;
    const void* base_addr;

    ElfW(Phdr)* phdrs;
    ElfW(Word) phdr_count;

    ElfW(Addr) load_bias;

    char* strs;

    ElfW(Sym)* syms;
    ElfW(Word) sym_count;
} semi_dlinfo_t;

// Copy from xhook.
static bool validate_elf_header(uintptr_t base_addr) {
    ElfW(Ehdr) *ehdr = (ElfW(Ehdr) *)base_addr;

    //check magic
    if(0 != memcmp(ehdr->e_ident, ELFMAG, SELFMAG)) return false;

    //check class (64/32)
#if defined(__LP64__)
    if(ELFCLASS64 != ehdr->e_ident[EI_CLASS]) return false;
#else
    if(ELFCLASS32 != ehdr->e_ident[EI_CLASS]) return false;
#endif

    //check endian (little/big)
    if(ELFDATA2LSB != ehdr->e_ident[EI_DATA]) return false;

    //check version
    if(EV_CURRENT != ehdr->e_ident[EI_VERSION]) return false;

    //check type
    if(ET_EXEC != ehdr->e_type && ET_DYN != ehdr->e_type) return false;

    //check machine
#if defined(__arm__)
    if(EM_ARM != ehdr->e_machine) return false;
#elif defined(__aarch64__)
    if(EM_AARCH64 != ehdr->e_machine) return false;
#elif defined(__i386__)
    if(EM_386 != ehdr->e_machine) return false;
#elif defined(__x86_64__)
    if(EM_X86_64 != ehdr->e_machine) return false;
#else
    return false;
#endif

    //check version
    if(EV_CURRENT != ehdr->e_version) return false;

    return true;
}

static int legacy_iterate_maps(iterate_callback cb, void* data) {
    int res = 0;
    FILE* fp = fopen("/proc/thread-self/maps", "r");
    if (fp == NULL) {
        LOGE(LOG_TAG, "Fail to open /proc/thread-self/maps.");
        return res;
    }
    char line[512] = {};
    while (fgets(line, sizeof(line), fp) != NULL) {
        uintptr_t base_addr = 0;
        char perm[5] = {};
        int offset = 0;
        int pathname_pos = 0;

        if (sscanf(line, "%"PRIxPTR"-%*" PRIxPTR " %4s %x %*x:%*x %*d%n", &base_addr, perm, &offset, &pathname_pos) != 3) {
            continue;
        }

        // check permission
        if (perm[0] != 'r' || perm[3] != 'p') {
            continue;
        }

        // check offset
        if(offset != 0) {
            continue;
        }

        // get pathname
        while(isspace(line[pathname_pos]) && pathname_pos < (int)(sizeof(line) - 1)) {
            pathname_pos += 1;
        }
        if (pathname_pos >= (int)(sizeof(line) - 1)) {
            continue;
        }
        char* pathname = line + pathname_pos;
        size_t pathname_len = strlen(pathname);
        if (pathname_len == 0) {
            continue;
        }
        if (pathname[pathname_len - 1] == '\n') {
            pathname[pathname_len - 1] = '\0';
            --pathname_len;
        }

        // check pathname
        if(pathname_len == 0 || pathname[0] == '[') {
            continue;
        }
        if (pathname_len >= 5 && strncmp(pathname, "/dev/", 5) == 0) {
            continue;
        }

        Dl_info stack_info;
        if (dladdr((const void*) base_addr, &stack_info) == 0) {
            LOGW(LOG_TAG, "'%s' is not loaded by linker, skip it.", pathname);
            continue;
        }

        // check baseaddr
        if (!validate_elf_header(base_addr)) {
            LOGW(LOG_TAG, "'%s' is not valid ELF file, skip it.", pathname);
            continue;
        }

        struct dl_phdr_info info;
        info.dlpi_addr = base_addr;
        info.dlpi_name = pathname;
        info.dlpi_phdr = (ElfW(Phdr)*) (base_addr + ((ElfW(Ehdr)*) base_addr)->e_phoff);
        info.dlpi_phnum = ((ElfW(Ehdr)*) base_addr)->e_phnum;
        res = cb(&info, sizeof(struct dl_phdr_info), data);

        if (res != 0) {
            break;
        }
    }

    fclose(fp);

    return res;
}

// Make compiler happy.
extern int dl_iterate_phdr(int (*__callback)(struct dl_phdr_info*, size_t, void*), void* __data) __attribute__((weak));

#ifdef __LP64__
#define LINKER_PATHNAME_SUFFIX "/system/bin/linker64"
#else
#define LINKER_PATHNAME_SUFFIX "/system/bin/linker"
#endif

#define LINKER_PATHNAME_SUFFIX_LEN (sizeof(LINKER_PATHNAME_SUFFIX) - 1)

static pthread_mutex_t s_linker_base_mutex = PTHREAD_MUTEX_INITIALIZER;
static const void* s_linker_base = NULL;
static pthread_mutex_t s_dl_mutex_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* s_dl_mutex_address = NULL;

static int find_linker_base_iter_cb(struct dl_phdr_info* info, size_t info_size, void* data) {
    const char* pathname = info->dlpi_name;
    size_t pathname_len = sizeof(pathname);
    if (pathname_len < LINKER_PATHNAME_SUFFIX_LEN) {
        return 0;
    }
    if (strncmp(pathname + pathname_len - LINKER_PATHNAME_SUFFIX_LEN,
            LINKER_PATHNAME_SUFFIX, LINKER_PATHNAME_SUFFIX_LEN) == 0) {
        *((const void**) data) = (const void*) info->dlpi_addr;
        return 1;
    }
    return 0;
}

static const void* get_linker_base_address() {
    pthread_mutex_lock(&s_linker_base_mutex);
    if (s_linker_base != NULL) {
        goto bail;
    }

    s_linker_base = (const void*) getauxval(AT_BASE);
    if (s_linker_base == NULL) {
        legacy_iterate_maps(find_linker_base_iter_cb, &s_linker_base);
    }

    bail:
    pthread_mutex_unlock(&s_linker_base_mutex);
    return s_linker_base;
}

#define LINKER_DL_MUTEX_SYMNAME "__dl__ZL10g_dl_mutex"

static pthread_mutex_t* get_dl_mutex_address() {
    pthread_mutex_lock(&s_dl_mutex_mutex);
    if (s_dl_mutex_address != NULL) {
        goto bail;
    }

    const void* linker_base = get_linker_base_address();
    if (linker_base == NULL) {
        goto bail;
    }

    s_dl_mutex_address = semi_dlsym(linker_base, LINKER_DL_MUTEX_SYMNAME);

    bail:
    pthread_mutex_unlock(&s_dl_mutex_mutex);
    return s_dl_mutex_address;
}

int semi_dl_iterate_phdr(iterate_callback cb, void* data) {
    int sdk = android_get_device_api_level();
    if (sdk < 21) {
        return legacy_iterate_maps(cb, data);
    } else if (sdk >= 21 && sdk <= 22) {
        pthread_mutex_t *dl_mutex = get_dl_mutex_address();
        if (dl_mutex != NULL) {
            pthread_mutex_lock(dl_mutex);
        }
        const void *linker_base = get_linker_base_address();
        int ret = 0;
        if (linker_base != NULL) {
            struct dl_phdr_info dlinfo;
            dlinfo.dlpi_addr = (ElfW(Addr)) linker_base;
            dlinfo.dlpi_name = LINKER_PATHNAME_SUFFIX;
            ElfW(Ehdr)* ehdr = (ElfW(Ehdr)*) linker_base;
            dlinfo.dlpi_phdr = (ElfW(Phdr)*) (((uintptr_t) linker_base) + ehdr->e_phoff);
            dlinfo.dlpi_phnum = ehdr->e_phnum;
            ret = cb(&dlinfo, sizeof(struct dl_phdr_info), data);
        } else {
            LOGW(LOG_TAG, "Cannot find base of linker.");
        }
        if (ret != 0) {
            goto bail_sdk_21_22;
        }

        ret = dl_iterate_phdr(cb, data);

        bail_sdk_21_22:
        if (dl_mutex != NULL) {
            pthread_mutex_unlock(dl_mutex);
        }
        return ret;
    } else if (sdk >= 23 && sdk <= 26) {
        const void *linker_base = get_linker_base_address();
        int ret = 0;
        if (linker_base != NULL) {
            struct dl_phdr_info dlinfo;
            dlinfo.dlpi_addr = (ElfW(Addr)) linker_base;
            dlinfo.dlpi_name = LINKER_PATHNAME_SUFFIX;
            ElfW(Ehdr)* ehdr = (ElfW(Ehdr)*) linker_base;
            dlinfo.dlpi_phdr = (ElfW(Phdr)*) (((uintptr_t) linker_base) + ehdr->e_phoff);
            dlinfo.dlpi_phnum = ehdr->e_phnum;
            ret = cb(&dlinfo, sizeof(struct dl_phdr_info), data);
        } else {
            LOGW(LOG_TAG, "Cannot find base of linker.");
        }
        if (ret != 0) {
            goto bail_sdk_23_26;
        }

        ret = dl_iterate_phdr(cb, data);

        bail_sdk_23_26:
        return ret;
    } else {
        return dl_iterate_phdr(cb, data);
    }
}

typedef struct {
    const char* name_suffix;
    size_t name_suffix_len;
    semi_dlinfo_t* semi_dlinfo;
} semi_dlopen_iter_info;

static int dlopen_iter_cb(struct dl_phdr_info* info, size_t info_size, void* data) {
    semi_dlopen_iter_info* iter_info = (semi_dlopen_iter_info*) data;
    semi_dlinfo_t* semi_dlinfo = iter_info->semi_dlinfo;

    const char* pathname = info->dlpi_name;
    if (pathname == NULL) {
        return 0;
    }
    size_t pathname_len = strlen(pathname);
    const char* name_suffix = iter_info->name_suffix;
    size_t name_suffix_len = iter_info->name_suffix_len;
    if (pathname_len < name_suffix_len) {
        return 0;
    }

    LOGD(LOG_TAG, "dlopen_iter_cb, pathname: %s, name_suffix: %s, suffix_len: %zu", pathname, name_suffix, name_suffix_len);

    if (strncmp(pathname + pathname_len - name_suffix_len, name_suffix, name_suffix_len) == 0) {
        semi_dlinfo->pathname = pathname;
        semi_dlinfo->base_addr = (const void*) info->dlpi_addr;
        ElfW(Ehdr)* ehdr = (ElfW(Ehdr)*) info->dlpi_addr;
        semi_dlinfo->phdrs = (ElfW(Phdr)*) (((uintptr_t) info->dlpi_addr) + ehdr->e_phoff);
        semi_dlinfo->phdr_count = ehdr->e_phnum;
        return 1;
    }

    return 0;
}

static bool load_section(int fd, off_t file_offset, size_t section_size, void** section_out) {
    ElfW(Off) aligned_file_offset = (file_offset & (~(getpagesize() - 1)));
    off_t bias = file_offset - aligned_file_offset;
    size_t map_size = section_size + getpagesize();
    void* mapped_section = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, aligned_file_offset);
    bool res = true;
    if (LIKELY(mapped_section != MAP_FAILED)) {
        *section_out = malloc(section_size);
        if (UNLIKELY(*section_out == NULL)) {
            LOGE(LOG_TAG, "Fail to allocate space for loading section.");
            goto fail;
        }
        uintptr_t mapped_section_start = ((uintptr_t) mapped_section) + bias;
        memcpy(*section_out, (const void*) mapped_section_start, section_size);
        goto bail;
    } else {
        int errcode = errno;
        LOGE(LOG_TAG, "Fail to mmap file, error: %s", strerror(errcode));
        goto fail;
    }

    goto bail;

    fail:
    res = false;

    bail:
    if (LIKELY(mapped_section != MAP_FAILED)) {
        munmap(mapped_section, map_size);
    }
    return res;
}

static bool fill_rest_neccessary_data(semi_dlinfo_t* semi_dlinfo) {
    ElfW(Ehdr)* ehdr = (ElfW(Ehdr)*) semi_dlinfo->base_addr;
    ElfW(Phdr)* phdrs = semi_dlinfo->phdrs;
    for (int i = 0; i < semi_dlinfo->phdr_count; ++i) {
        if (phdrs[i].p_type == PT_LOAD) {
            if ((ElfW(Addr)) semi_dlinfo->base_addr < phdrs[i].p_vaddr) {
                LOGE(LOG_TAG, "Unexpected first program header.");
                return false;
            }
            semi_dlinfo->load_bias = ((uintptr_t) semi_dlinfo->base_addr) - phdrs[i].p_vaddr;
            break;
        }
    }

    int fd = open(semi_dlinfo->pathname, O_RDONLY);
    if (fd < 0) {
        LOGE(LOG_TAG, "Fail to open \"%s\".", semi_dlinfo->pathname);
        return false;
    }

    ElfW(Shdr)* shdrs = NULL;
    if (!load_section(fd, ehdr->e_shoff, ehdr->e_shentsize * ehdr->e_shnum, (void**) &shdrs)) {
        LOGE(LOG_TAG, "Fail to map section headers of \"%s\".", semi_dlinfo->pathname);
        close(fd);
        return false;
    }

    bool strtbl_ok = false;
    bool symtbl_ok = false;
    for (int shdr_idx = ehdr->e_shnum - 1; shdr_idx >= 0; --shdr_idx) {
        ElfW(Shdr)* shdr = shdrs + shdr_idx;
        if (shdr->sh_type == SHT_STRTAB && shdr_idx != ehdr->e_shstrndx) {
            // Load symbol name table but not section name table.
            LOGD(LOG_TAG, "load strtab, sh_off: %" PRIxPTR ", sh_size: %" PRIuPTR,
                    (uintptr_t) shdr->sh_offset, (uintptr_t) shdr->sh_size);
            if (LIKELY(load_section(fd, shdr->sh_offset, shdr->sh_size, (void **) &semi_dlinfo->strs))) {
                strtbl_ok = true;
            } else {
                LOGE(LOG_TAG, "Fail to map string table of \"%s\"", semi_dlinfo->pathname);
                semi_dlinfo->strs = NULL;
                strtbl_ok = false;
            }
        } else if (shdr->sh_type == SHT_SYMTAB) {
            LOGD(LOG_TAG, "load symtab, sh_off: %" PRIxPTR ", sh_size: %" PRIuPTR,
                 (uintptr_t) shdr->sh_offset, (uintptr_t) shdr->sh_size);
            if (LIKELY(load_section(fd, shdr->sh_offset, shdr->sh_size, (void **) &semi_dlinfo->syms))) {
                semi_dlinfo->sym_count = shdr->sh_size / shdr->sh_entsize;
                symtbl_ok = true;
            } else {
                LOGE(LOG_TAG, "Fail to map symbol table of \"%s\"", semi_dlinfo->pathname);
                semi_dlinfo->syms = NULL;
                semi_dlinfo->sym_count = 0;
                symtbl_ok = false;
            }
        }
        if (strtbl_ok && symtbl_ok) {
            break;
        }
    }

    close(fd);
    if (shdrs != NULL) {
        free(shdrs);
    }

    if (LIKELY(strtbl_ok && symtbl_ok)) {
        return true;
    } else {
        LOGE(LOG_TAG, "Failure in fill_rest_neccessary_data.");
        return false;
    }
}

void* semi_dlopen(const char* pathname) {
    if (pathname == NULL) {
        LOGE(LOG_TAG, "pathname is null.");
        return NULL;
    }
    size_t pathname_len = strlen(pathname);
    if (pathname_len == 0) {
        LOGE(LOG_TAG, "pathname is empty.");
        return NULL;
    }
    char* name_suffix = (char*) pathname;
    size_t name_suffix_len = pathname_len;
    if (pathname[0] != '/') {
        size_t suffix_buffer_size = pathname_len + 1 /* slash */ + 1 /* EOL */;
        name_suffix = (char*) malloc(suffix_buffer_size);
        if (name_suffix == NULL) {
            LOGE(LOG_TAG, "Cannot allocate space for name suffix.");
            return NULL;
        }
        snprintf(name_suffix, suffix_buffer_size, "/%s", pathname);
        ++name_suffix_len;
    }

    semi_dlinfo_t* result = (semi_dlinfo_t*) malloc(sizeof(semi_dlinfo_t));
    if (result == NULL) {
        LOGE(LOG_TAG, "Cannot allocate space for semi_dlinfo.");
        goto bail;
    }

    memset(result, 0, sizeof(semi_dlinfo_t));
    result->magic = SEMI_DLINFO_MAGIC;

    semi_dlopen_iter_info iter_info = {
            .name_suffix = name_suffix,
            .name_suffix_len = name_suffix_len,
            .semi_dlinfo = result
    };

    semi_dl_iterate_phdr(dlopen_iter_cb, &iter_info);

    if (result->base_addr != NULL) {
        if (!fill_rest_neccessary_data(result)) {
            goto fail;
        }
    } else {
        LOGE(LOG_TAG, "Library with name ends with \"%s\" is not loaded by system before.", name_suffix);
        goto fail;
    }

    goto bail;

    fail:
    if (result != NULL) {
        free(result);
        result = NULL;
    }

    bail:
    if (name_suffix != NULL && name_suffix != pathname) {
        free(name_suffix);
    }

    return result;
}

void* semi_dlsym(const void* semi_hlib, const char* sym_name) {
    semi_dlinfo_t* semi_dlinfo = (semi_dlinfo_t*) semi_hlib;
    if (UNLIKELY(semi_dlinfo->magic != SEMI_DLINFO_MAGIC)) {
        LOGE(LOG_TAG, "Invalid semi_hlib, skip doing dlsym.");
        return NULL;
    }

    for (int i = 0; i < semi_dlinfo->sym_count; ++i) {
        ElfW(Sym) *curr_sym = semi_dlinfo->syms + i;
        int sym_type = ELF_ST_TYPE(curr_sym->st_info);
        const char *curr_sym_name = semi_dlinfo->strs + curr_sym->st_name;
        if ((sym_type == STT_FUNC || sym_type == STT_OBJECT) && strcmp(curr_sym_name, sym_name) == 0) {
            return (void *) semi_dlinfo->load_bias + curr_sym->st_value;
        }
    }

    LOGE(LOG_TAG, "Cannot find symbol \"%s\" in \"%s\"", sym_name, semi_dlinfo->pathname);
    return NULL;
}

void semi_dlclose(void* semi_hlib) {
    if (UNLIKELY(semi_hlib == NULL)) {
        LOGE(LOG_TAG, "semi_hlib is null.");
        return;
    }
    semi_dlinfo_t* semi_dlinfo = (semi_dlinfo_t*) semi_hlib;
    if (UNLIKELY(semi_dlinfo->magic != SEMI_DLINFO_MAGIC)) {
        LOGE(LOG_TAG, "Invalid semi_hlib, skip closing.");
        return;
    }
    if (semi_dlinfo->strs != NULL) {
        free(semi_dlinfo->strs);
    }
    if (semi_dlinfo->syms != NULL) {
        free(semi_dlinfo->syms);
    }
    free(semi_hlib);
}