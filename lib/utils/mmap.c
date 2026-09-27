#include "utils/mmap.h"

#define MMAP_SHM_NAMEMAX 31 // 共享内存名字总长上限（macOS 的 PSHMNAMLEN，各平台统一按它拦）
#define MMAP_FILE_PERM 0644 // 默认文件权限
#define MMAP_SHM_PERM 0600 // 默认共享内存权限
#ifdef OS_WIN
#define MMAP_INVALID_FD INVALID_HANDLE_VALUE // 无效句柄
#define MMAP_NT_VIEWUNMAP 2 // NtMapViewOfSection 的 SECTION_INHERIT::ViewUnmap
#else
#define MMAP_INVALID_FD -1 // 无效句柄
#endif

typedef enum _mmap_kind {
    _MMAP_NONE = 0x00,
    _MMAP_FILE,
    _MMAP_ANON,
    _MMAP_SHM,
}_mmap_kind;
typedef int32_t(*_mmap_op)(mmap_ctx *ctx, char *pa, size_t plen, int32_t arg);// 对一段按页对齐的区间做的操作
#ifdef OS_WIN
typedef LONG(NTAPI *_nt_create_section)(PHANDLE, ACCESS_MASK, void *, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
typedef LONG(NTAPI *_nt_map_view)(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);
typedef ULONG(NTAPI *_nt_status2dos)(LONG);
#endif

static const mmap_opts _mmap_zero_opts;
static atomic_t _mmap_ps;// 页大小缓存，0 表示还没取
static atomic_t _mmap_gran;// 分配粒度缓存，0 表示还没取
#ifdef OS_WIN
static _nt_create_section _mmap_nt_create;
static _nt_map_view _mmap_nt_map;
static _nt_status2dos _mmap_nt_todos;
static atomic_t _mmap_nt_once;// ntdll 函数取址状态：0=未取 1=取中 2=已取
#endif

// 自己做的参数检查失败：错误码不来自系统调用，得自己补
static int32_t _mmap_inval(void) {
#ifdef OS_WIN
    SetLastError(ERROR_INVALID_PARAMETER);
#else
    errno = EINVAL;
#endif
    return ERR_FAILED;
}
// 页大小与分配粒度只取一次：两个各自独立的整数，并发首次调用各写一遍同样的值无妨
static void _mmap_sysinfo(void) {
#ifdef OS_WIN
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ATOMIC_SET_RELAXED(&_mmap_gran, (atomic_t)si.dwAllocationGranularity);
    ATOMIC_SET_RELAXED(&_mmap_ps, (atomic_t)si.dwPageSize);
#else
    atomic_t ps = (atomic_t)sysconf(_SC_PAGESIZE);
    ATOMIC_SET_RELAXED(&_mmap_gran, ps);
    ATOMIC_SET_RELAXED(&_mmap_ps, ps);
#endif
}
static size_t _mmap_pagesize(void) {
    if (0 == ATOMIC_GET_RELAXED(&_mmap_ps)) {
        _mmap_sysinfo();
    }
    return ATOMIC_GET_RELAXED(&_mmap_ps);
}
size_t mmap_granularity(void) {
    if (0 == ATOMIC_GET_RELAXED(&_mmap_gran)) {
        _mmap_sysinfo();
    }
    return ATOMIC_GET_RELAXED(&_mmap_gran);
}
static void _mmap_init(mmap_ctx *ctx) {
    ZERO(ctx, sizeof(mmap_ctx));
    ctx->fd = MMAP_INVALID_FD;
}
// 失败收尾：关掉已打开的句柄并复位，保住触发失败的那个错误码
static int32_t _mmap_fail(mmap_ctx *ctx) {
#ifdef OS_WIN
    DWORD err = GetLastError();
    mmap_close(ctx);
    SetLastError(err);
#else
    int32_t err = errno;
    mmap_close(ctx);
    errno = err;
#endif
    return ERR_FAILED;
}
static int32_t _mmap_mode_ok(int32_t mode) {
    return MMAP_RDONLY == mode || MMAP_RDWR == mode || MMAP_COPY == mode;
}
// 对相对 addr 的 [off, off + lens) 做 op：lens 为 0 表示到 size 末尾，区间按页对齐后交给 op，空区间直接成功。
// resize 失败后 addr 可能为 NULL 而 size 仍是旧值（留着给 close 截回用），越界判断见 mmap_in_range
static int32_t _mmap_apply(mmap_ctx *ctx, size_t off, size_t lens, _mmap_op op, int32_t arg) {
    size_t head;
    char *a;
    if (0 == lens && off <= ctx->size) {
        lens = ctx->size - off;
    }
    if (_MMAP_NONE == ctx->kind || !mmap_in_range(ctx, off, lens)) {
        return _mmap_inval();
    }
    if (0 == lens) {
        return ERR_OK;
    }
    a = ctx->addr + off;
    head = (size_t)((uintptr_t)a & (_mmap_pagesize() - 1));
    return op(ctx, a - head, lens + head, arg);
}
static int32_t _mmap_flen(mmap_ctx *ctx, uint64_t *len) {
#ifdef OS_WIN
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(ctx->fd, &sz)) {
        return ERR_FAILED;
    }
    *len = (uint64_t)sz.QuadPart;
#else
    struct stat st;
    if (0 != fstat(ctx->fd, &st)) {
        return ERR_FAILED;
    }
    *len = (uint64_t)st.st_size;
#endif
    return ERR_OK;
}
#ifndef OS_WIN
// 32 位编译时 off_t 可能只有 32 位
static int32_t _mmap_off_ok(uint64_t v) {
    return v <= (uint64_t)INT64_MAX && (uint64_t)(off_t)v == v;
}
#endif
// 本映射要扩文件了：当前长度 cur 比自己设到过的最大长度还长，多出来的是别人写的，close 时不能截
static void _mmap_floor(mmap_ctx *ctx, uint64_t cur) {
    if (cur > ctx->hw) {
        ctx->flen0 = cur;
    }
}
// 把文件长度从 cur 设成 len 并记下本映射设到过的最大值；Windows 用不动文件指针的写法，句柄可能与调用方共享文件指针
static int32_t _mmap_setlen(mmap_ctx *ctx, uint64_t cur, uint64_t len) {
    if (len > cur) {
        _mmap_floor(ctx, cur);
    }
#ifdef OS_WIN
    FILE_END_OF_FILE_INFO eof;
    eof.EndOfFile.QuadPart = (LONGLONG)len;
    if (!SetFileInformationByHandle(ctx->fd, FileEndOfFileInfo, &eof, sizeof(eof))) {
        return ERR_FAILED;
    }
#else
    if (!_mmap_off_ok(len)) {
        return _mmap_inval();
    }
    if (0 != ftruncate(ctx->fd, (off_t)len)) {
        return ERR_FAILED;
    }
#endif
    if (len > ctx->hw) {
        ctx->hw = len;
    }
    return ERR_OK;
}
#ifndef OS_WIN
// 文件不短于 len，不够就扩。不超过 hw 时不必查：映射期间文件只增不减（契约 4）
static int32_t _mmap_ensure(mmap_ctx *ctx, uint64_t len) {
    uint64_t flen;
    if (len <= ctx->hw) {
        return ERR_OK;
    }
    if (ERR_OK != _mmap_flen(ctx, &flen)) {
        return ERR_FAILED;
    }
    if (flen >= len) {
        return ERR_OK;
    }
    return _mmap_setlen(ctx, flen, len);
}
#endif
// 只读 / 写时复制映射不能越过文件末尾
static int32_t _mmap_fits(mmap_ctx *ctx, size_t size) {
    uint64_t flen;
    if (ERR_OK != _mmap_flen(ctx, &flen)) {
        return ERR_FAILED;
    }
    if (UINT64_MAX - ctx->off < size || ctx->off + size > flen) {
        return _mmap_inval();
    }
    return ERR_OK;
}
static void _mmap_unmap(mmap_ctx *ctx) {
    char *base = ctx->addr - ctx->delta;
#ifdef OS_WIN
    if (_MMAP_ANON == ctx->kind) {
        VirtualFree(base, 0, MEM_RELEASE);
    } else {
        UnmapViewOfFile(base);
    }
#else
    munmap(base, ctx->delta + ctx->cap);
#endif
    ctx->addr = NULL;
}
// 契约 4：截回到 max(映射前长度, off + size)，文件比本映射设到过的最大长度还长就是别人写的，不动
static void _mmap_trim(mmap_ctx *ctx) {
    uint64_t flen, want = ctx->off + ctx->size;
    if (want < ctx->flen0) {
        want = ctx->flen0;
    }
    if (ERR_OK == _mmap_flen(ctx, &flen) && flen > want && flen <= ctx->hw) {
        (void)_mmap_setlen(ctx, flen, want);
    }
}
void mmap_close(mmap_ctx *ctx) {
    if (_MMAP_NONE == ctx->kind) {
        return;
    }
    if (NULL != ctx->addr) {
        _mmap_unmap(ctx);
    }
    if (_MMAP_FILE == ctx->kind && MMAP_RDWR == ctx->mode) {
        _mmap_trim(ctx);
    }
    if (MMAP_INVALID_FD != ctx->fd) {
#ifdef OS_WIN
        CloseHandle(ctx->fd);
#else
        close(ctx->fd);
#endif
    }
    _mmap_init(ctx);
}
#ifdef OS_WIN
static uint64_t _mmap_roundup64(uint64_t v, size_t align) {
    return (v + align - 1) & ~((uint64_t)align - 1);
}
static int32_t _mmap_prefetch(char *p, size_t lens) {
    WIN32_MEMORY_RANGE_ENTRY re;
    re.VirtualAddress = p;
    re.NumberOfBytes = lens;
    return PrefetchVirtualMemory(GetCurrentProcess(), 1, &re, 0) ? ERR_OK : ERR_FAILED;
}
// 普通视图：只读、写时复制、不预留的读写；读写时文件不够长由建映射对象一并扩到 end
static char *_mmap_win_view(mmap_ctx *ctx, uint64_t aoff, size_t len, uint64_t end) {
    DWORD prot, access, err;
    uint64_t maxsz = 0, flen;
    HANDLE mh;
    char *p;
    if (MMAP_RDONLY == ctx->mode) {
        prot = PAGE_READONLY;
        access = FILE_MAP_READ;
    } else if (MMAP_COPY == ctx->mode) {
        prot = PAGE_WRITECOPY;
        access = FILE_MAP_COPY;
    } else {
        prot = PAGE_READWRITE;
        access = FILE_MAP_WRITE;
        if (ERR_OK != _mmap_flen(ctx, &flen)) {
            return NULL;
        }
        if (end > flen) {
            maxsz = end;
            _mmap_floor(ctx, flen);
        }
    }
    mh = CreateFileMappingW(ctx->fd, NULL, prot, (DWORD)(maxsz >> 32), (DWORD)maxsz, NULL);
    if (NULL == mh) {
        return NULL;
    }
    if (maxsz > ctx->hw) {
        ctx->hw = maxsz;
    }
    p = (char *)MapViewOfFile(mh, access, (DWORD)(aoff >> 32), (DWORD)aoff, len);
    err = GetLastError();
    CloseHandle(mh);
    SetLastError(err);
    return p;
}
// ntdll 的三个函数只取一次：CAS 赢的一方取址，其余等它把状态置成 2
static void _mmap_nt_load(void) {
    HMODULE nt;
    if (2 == ATOMIC_GET(&_mmap_nt_once)) {
        return;
    }
    if (ATOMIC_CAS(&_mmap_nt_once, 0, 1)) {
        nt = GetModuleHandleW(L"ntdll.dll");
        _mmap_nt_create = (_nt_create_section)GetProcAddress(nt, "NtCreateSection");
        _mmap_nt_map = (_nt_map_view)GetProcAddress(nt, "NtMapViewOfSection");
        _mmap_nt_todos = (_nt_status2dos)GetProcAddress(nt, "RtlNtStatusToDosError");
        ATOMIC_CAS(&_mmap_nt_once, 1, 2);
        return;
    }
    while (2 != ATOMIC_GET(&_mmap_nt_once)) {
        CPU_PAUSE();
    }
}
// 可扩展视图：预留 len 字节地址，之后提交页时文件跟着变长。区段必须按 PAGE_READWRITE 建
static char *_mmap_win_xview(mmap_ctx *ctx, uint64_t aoff, size_t len) {
    HANDLE sec = NULL;
    LARGE_INTEGER off;
    SIZE_T vs = len;
    PVOID p = NULL;
    LONG st;
    _mmap_nt_load();
    if (NULL == _mmap_nt_create || NULL == _mmap_nt_map || NULL == _mmap_nt_todos) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }
    st = _mmap_nt_create(&sec, SECTION_MAP_READ | SECTION_MAP_WRITE, NULL, NULL, PAGE_READWRITE, SEC_RESERVE, ctx->fd);
    if (0 != st) {
        SetLastError(_mmap_nt_todos(st));
        return NULL;
    }
    off.QuadPart = (LONGLONG)aoff;
    st = _mmap_nt_map(sec, GetCurrentProcess(), &p, 0, 0, &off, &vs, MMAP_NT_VIEWUNMAP, MEM_RESERVE, PAGE_READWRITE);
    CloseHandle(sec);
    if (0 != st) {
        SetLastError(_mmap_nt_todos(st));
        return NULL;
    }
    return (char *)p;
}
// 建可扩展视图前把文件长度补到整页且不短于 end：此后增长都从页边界提交，末页里写的数据才落得进文件
static int32_t _mmap_win_pad(mmap_ctx *ctx, uint64_t end) {
    uint64_t flen, want;
    if (ERR_OK != _mmap_flen(ctx, &flen)) {
        return ERR_FAILED;
    }
    want = _mmap_roundup64(flen > end ? flen : end, _mmap_pagesize());
    if (want == flen) {
        return ERR_OK;
    }
    return _mmap_setlen(ctx, flen, want);
}
// cap 内变长：把 [文件末尾, want) 对应的页提交掉，文件随之按页变长
static int32_t _mmap_win_grow(mmap_ctx *ctx, uint64_t want) {
    uint64_t flen, aoff = ctx->off - ctx->delta;
    size_t ps = _mmap_pagesize(), from, to;
    char *base = ctx->addr - ctx->delta;
    if (want <= ctx->hw) {
        return ERR_OK;
    }
    if (ERR_OK != _mmap_flen(ctx, &flen)) {
        return ERR_FAILED;
    }
    if (want <= flen) {
        return ERR_OK;
    }
    from = (size_t)_mmap_roundup64(flen - aoff, ps);
    to = (size_t)_mmap_roundup64(want - aoff, ps);
    _mmap_floor(ctx, flen);
    if (NULL == VirtualAlloc(base + from, to - from, MEM_COMMIT, PAGE_READWRITE)) {
        return ERR_FAILED;
    }
    if (aoff + to > ctx->hw) {
        ctx->hw = aoff + to;
    }
    return ERR_OK;
}
#else
static int32_t _mmap_prot(int32_t mode) {
    return MMAP_RDONLY == mode ? PROT_READ : (PROT_READ | PROT_WRITE);
}
static int32_t _mmap_mflags(mmap_ctx *ctx) {
    int32_t mf = MMAP_COPY == ctx->mode ? MAP_PRIVATE : MAP_SHARED;
#ifdef MAP_POPULATE
    if (0 != (ctx->flags & MMAP_POPULATE)) {
        mf |= MAP_POPULATE;
    }
#endif
    return mf;
}
static char *_mmap_posix_map(mmap_ctx *ctx, uint64_t aoff, size_t len) {
    void *p;
    if (!_mmap_off_ok(aoff)) {
        _mmap_inval();
        return NULL;
    }
    p = mmap(NULL, len, _mmap_prot(ctx->mode), _mmap_mflags(ctx), ctx->fd, (off_t)aoff);
    return MAP_FAILED == p ? NULL : (char *)p;
}
// 超出 cap 的增长：Linux 用 mremap；其他平台先在原映射尾部接着映射，
// FreeBSD 开着 ASLR 会无视提示地址所以用 MAP_FIXED|MAP_EXCL，macOS 只能给提示地址再核对落点；都不成再整体重映射。
// 整体重映射时写时复制映射的私有改动要拷过去，否则新映射读到的是文件原内容
static char *_mmap_posix_regrow(mmap_ctx *ctx, size_t newlen) {
    char *base = ctx->addr - ctx->delta;
    size_t oldlen = ctx->delta + ctx->cap;
    uint64_t aoff = ctx->off - ctx->delta;
    void *p;
#if defined(OS_LINUX)
    p = mremap(base, oldlen, newlen, MREMAP_MAYMOVE);
    if (MAP_FAILED != p) {
        return (char *)p;
    }
#else
    size_t ext = ROUND_UP(oldlen, _mmap_pagesize());
    int32_t mf = _mmap_mflags(ctx);
    if (newlen <= ext) {
        return base;
    }
#if defined(OS_FBSD) && defined(MAP_EXCL)
    mf |= MAP_FIXED | MAP_EXCL;
#endif
    if (_mmap_off_ok(aoff + ext)) {
        p = mmap(base + ext, newlen - ext, _mmap_prot(ctx->mode), mf, ctx->fd, (off_t)(aoff + ext));
        if (MAP_FAILED != p) {
            if (base + ext == (char *)p) {
                return base;
            }
            munmap(p, newlen - ext);
        }
    }
#endif
    p = _mmap_posix_map(ctx, aoff, newlen);
    if (NULL == p) {
        return NULL;
    }
    if (MMAP_COPY == ctx->mode) {
        memcpy((char *)p + ctx->delta, ctx->addr, ctx->size);
    }
    munmap(base, oldlen);
    return (char *)p;
}
#ifndef MAP_POPULATE
static void _mmap_willneed(char *p, size_t lens) {
#if defined(OS_LINUX) || defined(OS_DARWIN) || defined(OS_BSD)
    (void)madvise(p, lens, MADV_WILLNEED);
#else
    (void)posix_madvise(p, lens, POSIX_MADV_WILLNEED);
#endif
}
#endif
#endif
// 从 addr 为 NULL 的状态建文件映射：[off - delta, off + cap)
static int32_t _mmap_file_map(mmap_ctx *ctx, size_t size, size_t cap) {
    uint64_t aoff = ctx->off - ctx->delta, end = ctx->off + size;
    size_t len = ctx->delta + cap;
    char *p;
    if (0 == size) {
        ctx->size = 0;
        ctx->cap = cap;
        return ERR_OK;
    }
#ifdef OS_WIN
    if (MMAP_RDWR == ctx->mode && cap > size) {
        if (ERR_OK != _mmap_win_pad(ctx, end)) {
            return ERR_FAILED;
        }
        p = _mmap_win_xview(ctx, aoff, len);
    } else {
        p = _mmap_win_view(ctx, aoff, len, end);
    }
    if (NULL == p) {
        return ERR_FAILED;
    }
    if (0 != (ctx->flags & MMAP_POPULATE)) {
        (void)_mmap_prefetch(p, ctx->delta + size);
    }
#else
    if (MMAP_RDWR == ctx->mode && ERR_OK != _mmap_ensure(ctx, end)) {
        return ERR_FAILED;
    }
    p = _mmap_posix_map(ctx, aoff, len);
    if (NULL == p) {
        return ERR_FAILED;
    }
#ifndef MAP_POPULATE
    if (0 != (ctx->flags & MMAP_POPULATE)) {
        _mmap_willneed(p, ctx->delta + size);
    }
#endif
#endif
    ctx->addr = p + ctx->delta;
    ctx->size = size;
    ctx->cap = cap;
    return ERR_OK;
}
// 按 opts 算出窗口并建映射；ctx 已持有文件句柄
static int32_t _mmap_file_setup(mmap_ctx *ctx, const mmap_opts *opts) {
    uint64_t flen;
    size_t size = opts->size, cap, gran = mmap_granularity();
    ctx->mode = opts->mode;
    ctx->flags = opts->flags;
    ctx->off = opts->off;
    if (ERR_OK != _mmap_flen(ctx, &flen)) {
        return ERR_FAILED;
    }
    ctx->flen0 = flen;
    ctx->hw = flen;
    if (0 == size) {
        if (opts->off > flen || (uint64_t)(size_t)(flen - opts->off) != flen - opts->off) {
            return _mmap_inval();
        }
        size = (size_t)(flen - opts->off);
    }
    cap = opts->cap < size ? size : opts->cap;
    if (MMAP_RDWR != ctx->mode && cap > size) {
        return _mmap_inval();
    }
    if (UINT64_MAX - opts->off < size || (MMAP_RDWR != ctx->mode && opts->off + size > flen)) {
        return _mmap_inval();
    }
    ctx->delta = (size_t)(opts->off & (gran - 1));
    if (cap > SIZE_MAX - ctx->delta) {
        return _mmap_inval();
    }
    return _mmap_file_map(ctx, size, cap);
}
int32_t mmap_open(mmap_ctx *ctx, const char *path, const mmap_opts *opts) {
    int32_t flags;
    _mmap_init(ctx);
    if (NULL == opts) {
        opts = &_mmap_zero_opts;
    }
    if (!_mmap_mode_ok(opts->mode)) {
        return _mmap_inval();
    }
    flags = opts->flags;
#ifdef OS_WIN
    DWORD access = GENERIC_READ | (MMAP_RDWR == opts->mode ? GENERIC_WRITE : 0);
    DWORD disp = OPEN_EXISTING;
    if (0 != (flags & MMAP_CREATE)) {
        disp = 0 != (flags & MMAP_EXCL) ? CREATE_NEW : OPEN_ALWAYS;
    }
    ctx->fd = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (MMAP_INVALID_FD == ctx->fd) {
        return ERR_FAILED;
    }
#else
    int32_t of = (MMAP_RDWR == opts->mode ? O_RDWR : O_RDONLY) | O_CLOEXEC;
    if (0 != (flags & MMAP_CREATE)) {
        of |= O_CREAT;
        if (0 != (flags & MMAP_EXCL)) {
            of |= O_EXCL;
        }
    }
    ctx->fd = open(path, of, (mode_t)(0 == opts->perm ? MMAP_FILE_PERM : opts->perm));
    if (MMAP_INVALID_FD == ctx->fd) {
        return ERR_FAILED;
    }
#endif
    ctx->kind = _MMAP_FILE;
    if (ERR_OK != _mmap_file_setup(ctx, opts)) {
        return _mmap_fail(ctx);
    }
    return ERR_OK;
}
int32_t mmap_map_fd(mmap_ctx *ctx, mmap_fd fd, const mmap_opts *opts) {
    _mmap_init(ctx);
    if (NULL == opts) {
        opts = &_mmap_zero_opts;
    }
    if (!_mmap_mode_ok(opts->mode)) {
        return _mmap_inval();
    }
#ifdef OS_WIN
    if (!DuplicateHandle(GetCurrentProcess(), fd, GetCurrentProcess(), &ctx->fd, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        ctx->fd = MMAP_INVALID_FD;
        return ERR_FAILED;
    }
#else
    ctx->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (MMAP_INVALID_FD == ctx->fd) {
        return ERR_FAILED;
    }
#endif
    ctx->kind = _MMAP_FILE;
    if (ERR_OK != _mmap_file_setup(ctx, opts)) {
        return _mmap_fail(ctx);
    }
    return ERR_OK;
}
// 匿名映射：预留 cap、提交前 size；失败返回 NULL
static char *_mmap_anon_alloc(size_t size, size_t cap, int32_t flags) {
    size_t csz = ROUND_UP(size, _mmap_pagesize());
#ifdef OS_WIN
    char *p;
    (void)flags;
    if (csz >= cap) {
        return (char *)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    p = (char *)VirtualAlloc(NULL, cap, MEM_RESERVE, PAGE_NOACCESS);
    if (NULL != p && 0 != csz && NULL == VirtualAlloc(p, csz, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return NULL;
    }
    return p;
#else
    int32_t mf = MAP_PRIVATE | MAP_ANONYMOUS;
    void *p;
    if (csz >= cap) {
#ifdef MAP_POPULATE
        if (0 != (flags & MMAP_POPULATE)) {
            mf |= MAP_POPULATE;
        }
#else
        (void)flags;
#endif
        p = mmap(NULL, cap, PROT_READ | PROT_WRITE, mf, -1, 0);
        return MAP_FAILED == p ? NULL : (char *)p;
    }
    p = mmap(NULL, cap, PROT_NONE, mf, -1, 0);
    if (MAP_FAILED == p) {
        return NULL;
    }
    if (0 != csz && 0 != mprotect(p, csz, PROT_READ | PROT_WRITE)) {
        munmap(p, cap);
        return NULL;
    }
    return (char *)p;
#endif
}
static int32_t _mmap_anon_map(mmap_ctx *ctx, size_t size, size_t cap) {
    char *p;
    if (0 == size) {
        ctx->size = 0;
        ctx->cap = cap;
        return ERR_OK;
    }
    p = _mmap_anon_alloc(size, cap, ctx->flags);
    if (NULL == p) {
        return ERR_FAILED;
    }
    ctx->addr = p;
    ctx->size = size;
    ctx->cap = cap;
    return ERR_OK;
}
int32_t mmap_anon(mmap_ctx *ctx, const mmap_opts *opts) {
    _mmap_init(ctx);
    if (NULL == opts) {
        opts = &_mmap_zero_opts;
    }
    ctx->kind = _MMAP_ANON;
    ctx->mode = MMAP_RDWR;
    ctx->flags = opts->flags;
    if (ERR_OK != _mmap_anon_map(ctx, opts->size, opts->cap < opts->size ? opts->size : opts->cap)) {
        return _mmap_fail(ctx);
    }
    return ERR_OK;
}
// cap 内提交或归还匿名内存：变长把新增页改成可读写，变短把多出的页换回不可访问的空页
static int32_t _mmap_anon_resize(mmap_ctx *ctx, size_t size) {
    size_t ps = _mmap_pagesize(), from = ROUND_UP(ctx->size, ps), to = ROUND_UP(size, ps);
    if (from == to) {
        return ERR_OK;
    }
#ifdef OS_WIN
    if (to > from) {
        return NULL == VirtualAlloc(ctx->addr + from, to - from, MEM_COMMIT, PAGE_READWRITE) ? ERR_FAILED : ERR_OK;
    }
    return VirtualFree(ctx->addr + to, from - to, MEM_DECOMMIT) ? ERR_OK : ERR_FAILED;
#else
    if (to > from) {
        return 0 == mprotect(ctx->addr + from, to - from, PROT_READ | PROT_WRITE) ? ERR_OK : ERR_FAILED;
    }
    return MAP_FAILED == mmap(ctx->addr + to, from - to, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) ? ERR_FAILED : ERR_OK;
#endif
}
// 超出 cap 的匿名映射：另建一块、拷过去、释放旧的
static int32_t _mmap_anon_regrow(mmap_ctx *ctx, size_t size, size_t newcap) {
    char *p = _mmap_anon_alloc(size, newcap, ctx->flags);
    if (NULL == p) {
        return ERR_FAILED;
    }
    memcpy(p, ctx->addr, ctx->size < size ? ctx->size : size);
    _mmap_unmap(ctx);
    ctx->addr = p;
    ctx->size = size;
    ctx->cap = newcap;
    return ERR_OK;
}
// 超出 cap 的文件映射：Windows 视图不能原地加长，先拆再建（写时复制映射要先建新的、拷过私有改动再拆）；
// POSIX 尽量接在尾部
static int32_t _mmap_file_regrow(mmap_ctx *ctx, size_t size, size_t newcap) {
#ifdef OS_WIN
    char *p;
    if (MMAP_COPY != ctx->mode) {
        _mmap_unmap(ctx);
        return _mmap_file_map(ctx, size, newcap);
    }
    p = _mmap_win_view(ctx, ctx->off - ctx->delta, ctx->delta + newcap, ctx->off + size);
    if (NULL == p) {
        return ERR_FAILED;
    }
    memcpy(p + ctx->delta, ctx->addr, ctx->size);
    _mmap_unmap(ctx);
    ctx->addr = p + ctx->delta;
    ctx->size = size;
    ctx->cap = newcap;
    return ERR_OK;
#else
    char *p;
    if (MMAP_RDWR == ctx->mode && ERR_OK != _mmap_ensure(ctx, ctx->off + size)) {
        return ERR_FAILED;
    }
    p = _mmap_posix_regrow(ctx, ctx->delta + newcap);
    if (NULL == p) {
        return ERR_FAILED;
    }
    ctx->addr = p + ctx->delta;
    ctx->size = size;
    ctx->cap = newcap;
    return ERR_OK;
#endif
}
// cap 内的文件映射：只有读写映射要让文件跟上
static int32_t _mmap_file_resize(mmap_ctx *ctx, size_t size) {
    if (MMAP_RDWR == ctx->mode) {
#ifdef OS_WIN
        if (ERR_OK != _mmap_win_grow(ctx, ctx->off + size)) {
            return ERR_FAILED;
        }
#else
        if (ERR_OK != _mmap_ensure(ctx, ctx->off + size)) {
            return ERR_FAILED;
        }
#endif
    }
    ctx->size = size;
    return ERR_OK;
}
int32_t mmap_resize(mmap_ctx *ctx, size_t size, size_t cap) {
    size_t newcap;
    if (_MMAP_FILE != ctx->kind && _MMAP_ANON != ctx->kind) {
        return _mmap_inval();
    }
    newcap = cap > ctx->cap ? cap : ctx->cap;
    if (newcap < size) {
        newcap = size;
    }
    if (_MMAP_FILE == ctx->kind) {
        if (MMAP_RDWR != ctx->mode) {
            if (cap > size) {
                return _mmap_inval();
            }
            if (ERR_OK != _mmap_fits(ctx, size)) {
                return ERR_FAILED;
            }
        } else if (UINT64_MAX - ctx->off < size) {
            return _mmap_inval();
        }
        if (newcap > SIZE_MAX - ctx->delta) {
            return _mmap_inval();
        }
    }
    if (NULL == ctx->addr) {
        return _MMAP_FILE == ctx->kind ? _mmap_file_map(ctx, size, newcap) : _mmap_anon_map(ctx, size, newcap);
    }
    if (newcap == ctx->cap) {
        if (_MMAP_FILE == ctx->kind) {
            return _mmap_file_resize(ctx, size);
        }
        if (ERR_OK != _mmap_anon_resize(ctx, size)) {
            return ERR_FAILED;
        }
        ctx->size = size;
        return ERR_OK;
    }
    return _MMAP_FILE == ctx->kind ? _mmap_file_regrow(ctx, size, newcap) : _mmap_anon_regrow(ctx, size, newcap);
}
// 共享内存名字："/名字"，名字里不再有 '/'，总长不超过 MMAP_SHM_NAMEMAX
static int32_t _mmap_shm_name_ok(const char *name) {
    size_t lens;
    if (NULL == name || '/' != name[0]) {
        return 0;
    }
    lens = strlen(name);
    return lens >= 2 && lens <= MMAP_SHM_NAMEMAX && NULL == strchr(name + 1, '/');
}
int32_t mmap_shm(mmap_ctx *ctx, const char *name, const mmap_opts *opts) {
    size_t size;
    int32_t create;
    _mmap_init(ctx);
    if (NULL == opts) {
        opts = &_mmap_zero_opts;
    }
    size = opts->size;
    create = 0 != (opts->flags & MMAP_CREATE);
    if ((MMAP_RDONLY != opts->mode && MMAP_RDWR != opts->mode) || !_mmap_shm_name_ok(name) || 0 != opts->off
        || (0 != opts->cap && opts->cap != size) || (create && 0 == size)) {
        return _mmap_inval();
    }
    ctx->kind = _MMAP_SHM;
    ctx->mode = opts->mode;
    ctx->flags = opts->flags;
#ifdef OS_WIN
    char aname[MMAP_SHM_NAMEMAX + 8];
    DWORD access = MMAP_RDONLY == opts->mode ? FILE_MAP_READ : FILE_MAP_WRITE;
    HANDLE mh;
    char *p;
    MEMORY_BASIC_INFORMATION mi;
    SNPRINTF(aname, sizeof(aname), "Local\\%s", name + 1);
    if (create) {
        SetLastError(ERROR_SUCCESS);
        mh = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32), (DWORD)size, aname);
        if (NULL != mh && ERROR_ALREADY_EXISTS == GetLastError() && 0 != (opts->flags & MMAP_EXCL)) {
            CloseHandle(mh);
            SetLastError(ERROR_ALREADY_EXISTS);
            return _mmap_fail(ctx);
        }
    } else {
        mh = OpenFileMappingA(MMAP_RDONLY == opts->mode ? FILE_MAP_READ : FILE_MAP_READ | FILE_MAP_WRITE, FALSE, aname);
    }
    if (NULL == mh) {
        return _mmap_fail(ctx);
    }
    ctx->fd = mh;
    p = (char *)MapViewOfFile(mh, access, 0, 0, size);
    if (NULL == p) {
        return _mmap_fail(ctx);
    }
    ctx->addr = p;
    VirtualQuery(p, &mi, sizeof(mi));
    if (0 == size) {
        size = mi.RegionSize;
    } else if (size > mi.RegionSize) {
        _mmap_inval();
        return _mmap_fail(ctx);
    }
#else
    uint64_t flen;
    void *p;
    int32_t of = (MMAP_RDWR == opts->mode || create ? O_RDWR : O_RDONLY) | (create ? O_CREAT : 0);
    if (create && 0 != (opts->flags & MMAP_EXCL)) {
        of |= O_EXCL;
    }
    ctx->fd = shm_open(name, of, (mode_t)(0 == opts->perm ? MMAP_SHM_PERM : opts->perm));
    if (MMAP_INVALID_FD == ctx->fd || ERR_OK != _mmap_flen(ctx, &flen)) {
        return _mmap_fail(ctx);
    }
    if (0 == flen) {
        if (!create || !_mmap_off_ok(size)) {
            _mmap_inval();
            return _mmap_fail(ctx);
        }
        if (0 != ftruncate(ctx->fd, (off_t)size) && (EINVAL != errno || ERR_OK != _mmap_flen(ctx, &flen) || flen < size)) {
            return _mmap_fail(ctx);
        }
        flen = size;
    }
    if (0 == size) {
        if ((uint64_t)(size_t)flen != flen) {
            _mmap_inval();
            return _mmap_fail(ctx);
        }
        size = (size_t)flen;
    }
    if (size > flen) {
        _mmap_inval();
        return _mmap_fail(ctx);
    }
    p = mmap(NULL, size, _mmap_prot(opts->mode), MAP_SHARED, ctx->fd, 0);
    if (MAP_FAILED == p) {
        return _mmap_fail(ctx);
    }
    ctx->addr = (char *)p;
#endif
    ctx->size = size;
    ctx->cap = size;
    return ERR_OK;
}
int32_t mmap_shm_unlink(const char *name) {
    if (!_mmap_shm_name_ok(name)) {
        return _mmap_inval();
    }
#ifdef OS_WIN
    return ERR_OK;
#else
    return 0 == shm_unlink(name) ? ERR_OK : ERR_FAILED;
#endif
}
static int32_t _mmap_op_sync(mmap_ctx *ctx, char *pa, size_t plen, int32_t async) {
    if (_MMAP_FILE != ctx->kind || MMAP_RDWR != ctx->mode) {
        return ERR_OK;
    }
#ifdef OS_WIN
    if (!FlushViewOfFile(pa, plen)) {
        return ERR_FAILED;
    }
    if (!async && !FlushFileBuffers(ctx->fd)) {
        return ERR_FAILED;
    }
#else
    if (0 != msync(pa, plen, async ? MS_ASYNC : MS_SYNC)) {
        return ERR_FAILED;
    }
#if defined(OS_DARWIN)
    if (!async && -1 == fcntl(ctx->fd, F_FULLFSYNC) && 0 != fsync(ctx->fd)) {
        return ERR_FAILED;
    }
#endif
#endif
    return ERR_OK;
}
int32_t mmap_sync(mmap_ctx *ctx, size_t off, size_t lens, int32_t async) {
    return _mmap_apply(ctx, off, lens, _mmap_op_sync, async);
}
static int32_t _mmap_op_advise(mmap_ctx *ctx, char *pa, size_t plen, int32_t advice) {
    (void)ctx;
#ifdef OS_WIN
    return MMAP_WILLNEED == advice ? _mmap_prefetch(pa, plen) : ERR_OK;
#elif defined(OS_LINUX) || defined(OS_DARWIN) || defined(OS_BSD)
    static const int32_t adv[] = { MADV_NORMAL, MADV_SEQUENTIAL, MADV_RANDOM, MADV_WILLNEED, MADV_DONTNEED };
    return 0 == madvise(pa, plen, adv[advice]) ? ERR_OK : ERR_FAILED;
#else
    static const int32_t adv[] = { POSIX_MADV_NORMAL, POSIX_MADV_SEQUENTIAL, POSIX_MADV_RANDOM, POSIX_MADV_WILLNEED, POSIX_MADV_DONTNEED };
    int32_t rtn = posix_madvise(pa, plen, adv[advice]);
    if (0 != rtn) {
        errno = rtn;
        return ERR_FAILED;
    }
    return ERR_OK;
#endif
}
int32_t mmap_advise(mmap_ctx *ctx, size_t off, size_t lens, int32_t advice) {
    if (advice < MMAP_NORMAL || advice > MMAP_DONTNEED) {
        return _mmap_inval();
    }
    if (MMAP_DONTNEED == advice && (_MMAP_ANON == ctx->kind || MMAP_COPY == ctx->mode)) {
        return _mmap_inval();
    }
    return _mmap_apply(ctx, off, lens, _mmap_op_advise, advice);
}
static int32_t _mmap_op_protect(mmap_ctx *ctx, char *pa, size_t plen, int32_t mode) {
#ifdef OS_WIN
    DWORD old, prot = PAGE_READONLY;
    if (MMAP_RDWR == mode) {
        prot = MMAP_COPY == ctx->mode ? PAGE_WRITECOPY : PAGE_READWRITE;
    }
    return VirtualProtect(pa, plen, prot, &old) ? ERR_OK : ERR_FAILED;
#else
    (void)ctx;
    return 0 == mprotect(pa, plen, _mmap_prot(mode)) ? ERR_OK : ERR_FAILED;
#endif
}
int32_t mmap_protect(mmap_ctx *ctx, size_t off, size_t lens, int32_t mode) {
    if ((MMAP_RDONLY != mode && MMAP_RDWR != mode) || (MMAP_RDWR == mode && MMAP_RDONLY == ctx->mode)) {
        return _mmap_inval();
    }
    return _mmap_apply(ctx, off, lens, _mmap_op_protect, mode);
}
static int32_t _mmap_op_lock(mmap_ctx *ctx, char *pa, size_t plen, int32_t lock) {
    (void)ctx;
#ifdef OS_WIN
    return (lock ? VirtualLock(pa, plen) : VirtualUnlock(pa, plen)) ? ERR_OK : ERR_FAILED;
#else
    return 0 == (lock ? mlock(pa, plen) : munlock(pa, plen)) ? ERR_OK : ERR_FAILED;
#endif
}
int32_t mmap_lock(mmap_ctx *ctx, size_t off, size_t lens) {
    return _mmap_apply(ctx, off, lens, _mmap_op_lock, 1);
}
int32_t mmap_unlock(mmap_ctx *ctx, size_t off, size_t lens) {
    return _mmap_apply(ctx, off, lens, _mmap_op_lock, 0);
}
int64_t mmap_filesize(mmap_ctx *ctx) {
    uint64_t flen;
    if (_MMAP_FILE != ctx->kind) {
        return _mmap_inval();
    }
    if (ERR_OK != _mmap_flen(ctx, &flen)) {
        return ERR_FAILED;
    }
    return (int64_t)flen;
}
