#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cctype>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <immintrin.h>


constexpr int    VDIM        = 14;
constexpr int    VDIM_PADDED = 16;
constexpr int    KNN_K       = 5;
constexpr size_t N_REFS      = 100000;

constexpr const char* DEFAULT_REFS_PATH   = "/refs.bin";
constexpr const char* DEFAULT_LABELS_PATH = "/labels.bin";

constexpr int  MAX_EVENTS = 256;
constexpr int  CONN_POOL  = 4096;
constexpr int  RBUF_SIZE  = 8192;
constexpr int  LISTEN_BACKLOG = 4096;


constexpr int16_t Q_SCALE    = 8192;
constexpr int16_t Q_SENTINEL = -8192;

static const int16_t* g_refs   = nullptr;
static const uint8_t* g_labels = nullptr;


struct Resp { const char* data; size_t len; };
static Resp g_resp_fraud[6];
static Resp g_resp_ready;
static Resp g_resp_400;
static Resp g_resp_404;


static void init_responses() {
    static const char* fraud[6] = {
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 33\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0}",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 34\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":1}",
    };
    for (int i = 0; i < 6; i++) {
        g_resp_fraud[i].data = fraud[i];
        g_resp_fraud[i].len  = strlen(fraud[i]);
    }
    static const char* ready = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
    g_resp_ready = { ready, strlen(ready) };
    static const char* bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    g_resp_400 = { bad, strlen(bad) };
    static const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    g_resp_404 = { nf, strlen(nf) };
}


static inline float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static inline float round4(float x) {
    return roundf(x * 10000.0f) / 10000.0f;
}

static inline float mcc_risk(uint32_t mcc) {
    switch (mcc) {
        case 5411: return 0.15f;
        case 5812: return 0.30f;
        case 5912: return 0.20f;
        case 5944: return 0.45f;
        case 7801: return 0.80f;
        case 7802: return 0.75f;
        case 7995: return 0.85f;
        case 4511: return 0.35f;
        case 5311: return 0.25f;
        case 5999: return 0.50f;
        default:   return 0.50f;
    }
}

static inline int parse_int2(const char* s) {
    return (s[0] - '0') * 10 + (s[1] - '0');
}

static int day_of_week(int y, int m, int d) {
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    int dow = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
    return (dow + 6) % 7;
}

static int64_t days_since_epoch(int y, int m, int d) {
    if (m <= 2) y -= 1;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int mm = (m > 2) ? m - 3 : m + 9;
    int doy = (153 * mm + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}


static const char* find_after(const char* buf, size_t len, const char* key, size_t klen) {
    const void* p = memmem(buf, len, key, klen);
    if (!p) return nullptr;
    const char* end = buf + len;
    const char* q = (const char*)p + klen;
    while (q < end && (*q == ':' || *q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
    return q;
}

static const char* find_section(const char* buf, size_t len, const char* key, size_t klen) {
    const void* p = memmem(buf, len, key, klen);
    return p ? (const char*)p : nullptr;
}

static float parse_num(const char* p, const char* end) {
    bool neg = false;
    if (p < end && *p == '-') { neg = true; p++; }
    double v = 0.0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10.0 + (*p - '0');
        p++;
    }
    if (p < end && *p == '.') {
        p++;
        double scale = 0.1;
        while (p < end && *p >= '0' && *p <= '9') {
            v += (*p - '0') * scale;
            scale *= 0.1;
            p++;
        }
    }
    return (float)(neg ? -v : v);
}

static int parse_int(const char* p, const char* end) {
    bool neg = false;
    if (p < end && *p == '-') { neg = true; p++; }
    int v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    return neg ? -v : v;
}

static uint32_t parse_digits_u32(const char* p, const char* end) {
    uint32_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (uint32_t)(*p - '0');
        p++;
    }
    return v;
}

struct IsoTs { int y, mo, d, h, mi, s; };

static bool parse_iso(const char* p, const char* end, IsoTs* out) {
    while (p < end && *p != '"') p++;
    if (p >= end) return false;
    p++;
    if (end - p < 19) return false;
    out->y  = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    out->mo = parse_int2(p + 5);
    out->d  = parse_int2(p + 8);
    out->h  = parse_int2(p + 11);
    out->mi = parse_int2(p + 14);
    out->s  = parse_int2(p + 17);
    return true;
}

static bool array_contains(const char* p, const char* end, const char* needle, size_t nlen) {
    while (p < end && *p != '[') p++;
    if (p >= end) return false;
    p++;
    while (p < end && *p != ']') {
        if (*p == '"') {
            const char* start = ++p;
            while (p < end && *p != '"') p++;
            if (p >= end) return false;
            if ((size_t)(p - start) == nlen && memcmp(start, needle, nlen) == 0) return true;
            p++;
        } else {
            p++;
        }
    }
    return false;
}


static inline int16_t quant(float v) {
    if (v < 0.0f) return 0;
    if (v > 1.0f) return Q_SCALE;
    int q = (int)(v * (float)Q_SCALE + 0.5f);
    if (q > Q_SCALE) q = Q_SCALE;
    return (int16_t)q;
}

static bool vectorize(const char* buf, size_t len, int16_t* out) {
    const char* tx_p   = find_section(buf, len, "\"transaction\"",      13);
    const char* cust_p = find_section(buf, len, "\"customer\"",         10);
    const char* mer_p  = find_section(buf, len, "\"merchant\"",         10);
    const char* term_p = find_section(buf, len, "\"terminal\"",         10);
    const char* last_p = find_section(buf, len, "\"last_transaction\"", 18);
    if (!tx_p || !cust_p || !mer_p || !term_p || !last_p) return false;

    struct { const char* p; int idx; } items[5] = {
        {tx_p, 0}, {cust_p, 1}, {mer_p, 2}, {term_p, 3}, {last_p, 4}
    };
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
            if (items[i].p > items[j].p) { auto t = items[i]; items[i] = items[j]; items[j] = t; }

    const char* secs[5][2];
    const char* end = buf + len;
    for (int i = 0; i < 5; i++) {
        secs[items[i].idx][0] = items[i].p;
        secs[items[i].idx][1] = (i + 1 < 5) ? items[i+1].p : end;
    }
    const char* tx_s = secs[0][0];   size_t tx_l   = secs[0][1] - tx_s;
    const char* cu_s = secs[1][0];   size_t cu_l   = secs[1][1] - cu_s;
    const char* me_s = secs[2][0];   size_t me_l   = secs[2][1] - me_s;
    const char* te_s = secs[3][0];   size_t te_l   = secs[3][1] - te_s;
    const char* la_s = secs[4][0];   size_t la_l   = secs[4][1] - la_s;

    const char* p;

    p = find_after(tx_s, tx_l, "\"amount\"", 8);          if (!p) return false;
    float amount = parse_num(p, end);

    p = find_after(tx_s, tx_l, "\"installments\"", 14);   if (!p) return false;
    int installments = parse_int(p, end);

    p = find_after(tx_s, tx_l, "\"requested_at\"", 14);   if (!p) return false;
    IsoTs req_ts;
    if (!parse_iso(p, end, &req_ts)) return false;

    p = find_after(cu_s, cu_l, "\"avg_amount\"", 12);     if (!p) return false;
    float cust_avg = parse_num(p, end);

    p = find_after(cu_s, cu_l, "\"tx_count_24h\"", 14);   if (!p) return false;
    int tx_count_24h = parse_int(p, end);

    p = find_after(me_s, me_l, "\"id\"", 4);              if (!p) return false;
    while (p < end && *p != '"') p++;
    if (p >= end) return false;
    const char* mid_s = ++p;
    while (p < end && *p != '"') p++;
    if (p >= end) return false;
    size_t mid_l = p - mid_s;

    p = find_after(me_s, me_l, "\"mcc\"", 5);             if (!p) return false;
    while (p < end && *p != '"') p++;
    if (p >= end) return false;
    p++;
    uint32_t mcc = parse_digits_u32(p, end);

    p = find_after(me_s, me_l, "\"avg_amount\"", 12);     if (!p) return false;
    float merch_avg = parse_num(p, end);

    p = find_after(te_s, te_l, "\"is_online\"", 11);      if (!p) return false;
    bool is_online = (*p == 't');

    p = find_after(te_s, te_l, "\"card_present\"", 14);   if (!p) return false;
    bool card_present = (*p == 't');

    p = find_after(te_s, te_l, "\"km_from_home\"", 14);   if (!p) return false;
    float km_home = parse_num(p, end);

    p = find_after(cu_s, cu_l, "\"known_merchants\"", 17); if (!p) return false;
    bool known = array_contains(p, end, mid_s, mid_l);

    bool has_last = false;
    int  last_minutes = 0;
    float last_km = 0.0f;
    {
        const char* lp = find_after(la_s, la_l, "\"last_transaction\"", 18);
        if (lp && lp < end) {
            if (lp[0] == '{') {
                has_last = true;
                const char* lts_p = find_after(la_s, la_l, "\"timestamp\"", 11);
                if (!lts_p) return false;
                IsoTs last_ts;
                if (!parse_iso(lts_p, end, &last_ts)) return false;
                int64_t d_req = days_since_epoch(req_ts.y, req_ts.mo, req_ts.d);
                int64_t d_lst = days_since_epoch(last_ts.y, last_ts.mo, last_ts.d);
                int64_t m_req = d_req * 1440 + req_ts.h * 60 + req_ts.mi;
                int64_t m_lst = d_lst * 1440 + last_ts.h * 60 + last_ts.mi;
                int64_t diff = m_req - m_lst;
                if (diff < 0) diff = -diff;
                last_minutes = (int)diff;

                const char* lkm_p = find_after(la_s, la_l, "\"km_from_current\"", 17);
                if (!lkm_p) return false;
                last_km = parse_num(lkm_p, end);
            }
        }
    }

    float fv[VDIM];
    fv[0] = clamp01(amount / 10000.0f);
    fv[1] = clamp01((float)installments / 12.0f);
    fv[2] = cust_avg > 0.0f ? clamp01((amount / cust_avg) / 10.0f) : 0.0f;
    fv[3] = (float)req_ts.h / 23.0f;
    fv[4] = (float)day_of_week(req_ts.y, req_ts.mo, req_ts.d) / 6.0f;
    if (has_last) {
        fv[5] = clamp01((float)last_minutes / 1440.0f);
        fv[6] = clamp01(last_km / 1000.0f);
    } else {
        fv[5] = -1.0f;
        fv[6] = -1.0f;
    }
    fv[7]  = clamp01(km_home / 1000.0f);
    fv[8]  = clamp01((float)tx_count_24h / 20.0f);
    fv[9]  = is_online ? 1.0f : 0.0f;
    fv[10] = card_present ? 1.0f : 0.0f;
    fv[11] = known ? 0.0f : 1.0f;
    fv[12] = mcc_risk(mcc);
    fv[13] = clamp01(merch_avg / 10000.0f);

    for (int i = 0; i < VDIM; i++) fv[i] = round4(fv[i]);

    for (int i = 0; i < VDIM; i++) {
        if ((i == 5 || i == 6) && fv[i] < 0.0f) {
            out[i] = Q_SENTINEL;
        } else {
            out[i] = quant(fv[i]);
        }
    }
    out[14] = 0;
    out[15] = 0;
    return true;
}


static inline void top_insert(uint32_t* td, uint32_t* ti, uint32_t d, uint32_t i) {
    int j = KNN_K - 1;
    while (j > 0 && td[j - 1] > d) {
        td[j] = td[j - 1];
        ti[j] = ti[j - 1];
        j--;
    }
    td[j] = d;
    ti[j] = i;
}

__attribute__((target("avx2")))
static int knn_avx2(const int16_t* vec) {
    const __m256i q = _mm256_loadu_si256((const __m256i*)vec);

    uint32_t td[KNN_K];
    uint32_t ti[KNN_K];
    for (int i = 0; i < KNN_K; i++) { td[i] = UINT32_MAX; ti[i] = 0; }
    uint32_t threshold = UINT32_MAX;

    for (size_t i = 0; i < N_REFS; i += 2) {
        if (i + 16 < N_REFS) _mm_prefetch((const char*)(g_refs + (i + 16) * VDIM_PADDED), _MM_HINT_T0);

        __m256i r0 = _mm256_load_si256((const __m256i*)(g_refs + (i + 0) * VDIM_PADDED));
        __m256i r1 = _mm256_load_si256((const __m256i*)(g_refs + (i + 1) * VDIM_PADDED));
        __m256i d0 = _mm256_sub_epi16(q, r0);
        __m256i d1 = _mm256_sub_epi16(q, r1);
        __m256i sq0 = _mm256_madd_epi16(d0, d0);
        __m256i sq1 = _mm256_madd_epi16(d1, d1);

        __m128i s0 = _mm_add_epi32(_mm256_castsi256_si128(sq0), _mm256_extracti128_si256(sq0, 1));
        s0 = _mm_add_epi32(s0, _mm_srli_si128(s0, 8));
        s0 = _mm_add_epi32(s0, _mm_srli_si128(s0, 4));
        uint32_t dist0 = (uint32_t)_mm_cvtsi128_si32(s0);

        __m128i s1 = _mm_add_epi32(_mm256_castsi256_si128(sq1), _mm256_extracti128_si256(sq1, 1));
        s1 = _mm_add_epi32(s1, _mm_srli_si128(s1, 8));
        s1 = _mm_add_epi32(s1, _mm_srli_si128(s1, 4));
        uint32_t dist1 = (uint32_t)_mm_cvtsi128_si32(s1);

        if (dist0 < threshold) {
            top_insert(td, ti, dist0, (uint32_t)(i + 0));
            threshold = td[KNN_K - 1];
        }
        if (dist1 < threshold) {
            top_insert(td, ti, dist1, (uint32_t)(i + 1));
            threshold = td[KNN_K - 1];
        }
    }

    return g_labels[ti[0]] + g_labels[ti[1]] + g_labels[ti[2]] + g_labels[ti[3]] + g_labels[ti[4]];
}

static int knn_scalar(const int16_t* vec) {
    uint32_t td[KNN_K];
    uint32_t ti[KNN_K];
    for (int i = 0; i < KNN_K; i++) { td[i] = UINT32_MAX; ti[i] = 0; }
    uint32_t threshold = UINT32_MAX;

    for (size_t i = 0; i < N_REFS; i++) {
        const int16_t* r = g_refs + i * VDIM_PADDED;
        uint32_t d = 0;
        for (int k = 0; k < VDIM; k++) {
            int32_t diff = (int32_t)vec[k] - (int32_t)r[k];
            d += (uint32_t)(diff * diff);
        }
        if (d < threshold) {
            top_insert(td, ti, d, (uint32_t)i);
            threshold = td[KNN_K - 1];
        }
    }

    return g_labels[ti[0]] + g_labels[ti[1]] + g_labels[ti[2]] + g_labels[ti[3]] + g_labels[ti[4]];
}

typedef int (*knn_fn)(const int16_t*);
static knn_fn g_knn = nullptr;


static const void* mmap_file(const char* path, size_t expected) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); exit(1); }
    if ((size_t)st.st_size != expected) {
        fprintf(stderr, "%s size: got %lld expected %zu\n",
                path, (long long)st.st_size, expected);
        exit(1);
    }
    void* p = mmap(nullptr, expected, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    return p;
}


struct Conn {
    int   fd;
    bool  in_use;
    bool  want_out;
    char  rb[RBUF_SIZE];
    size_t rp;
    const char* wb;
    size_t wlen;
    size_t wp;
};

static Conn  g_pool[CONN_POOL];
static int   g_free_list[CONN_POOL];
static int   g_free_top;
static int   g_epfd;


static void pool_init() {
    for (int i = 0; i < CONN_POOL; i++) {
        g_pool[i].in_use = false;
        g_free_list[i] = CONN_POOL - 1 - i;
    }
    g_free_top = CONN_POOL;
}

static Conn* conn_alloc(int fd) {
    if (g_free_top == 0) return nullptr;
    Conn* c = &g_pool[g_free_list[--g_free_top]];
    c->fd       = fd;
    c->in_use   = true;
    c->want_out = false;
    c->rp       = 0;
    c->wb       = nullptr;
    c->wlen     = 0;
    c->wp       = 0;
    return c;
}

static void conn_free(Conn* c) {
    if (!c->in_use) return;
    int idx = c - g_pool;
    c->in_use = false;
    g_free_list[g_free_top++] = idx;
}

static void conn_close(Conn* c) {
    if (!c->in_use) return;
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, nullptr);
    close(c->fd);
    conn_free(c);
}

static void epoll_mod(Conn* c, bool want_out) {
    if (c->want_out == want_out) return;
    c->want_out = want_out;
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | (want_out ? EPOLLOUT : 0);
    ev.data.ptr = c;
    epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
}


static void try_send(Conn* c) {
    while (c->wp < c->wlen) {
        ssize_t n = send(c->fd, c->wb + c->wp, c->wlen - c->wp, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                epoll_mod(c, true);
                return;
            }
            conn_close(c);
            return;
        }
        c->wp += (size_t)n;
    }
    c->wb = nullptr;
    c->wlen = 0;
    c->wp = 0;
    epoll_mod(c, false);
}


static int try_process_one(Conn* c) {
    if (c->rp < 16) return 0;

    const void* hep = memmem(c->rb, c->rp, "\r\n\r\n", 4);
    if (!hep) {
        if (c->rp >= RBUF_SIZE - 1) return -1;
        return 0;
    }
    size_t headers_end = (const char*)hep - c->rb + 4;

    if (c->rp >= 18 && memcmp(c->rb, "POST /fraud-score ", 18) == 0) {
        const void* clp = memmem(c->rb, headers_end, "Content-Length:", 15);
        if (!clp) {
            c->wb = g_resp_400.data; c->wlen = g_resp_400.len; c->wp = 0;
            size_t consumed = headers_end;
            memmove(c->rb, c->rb + consumed, c->rp - consumed);
            c->rp -= consumed;
            return 1;
        }
        const char* p = (const char*)clp + 15;
        const char* end = c->rb + headers_end;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        size_t content_len = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            content_len = content_len * 10 + (size_t)(*p - '0');
            p++;
        }
        size_t body_end = headers_end + content_len;
        if (body_end > RBUF_SIZE) return -1;
        if (c->rp < body_end) return 0;

        alignas(32) int16_t vec[VDIM_PADDED];
        const Resp* resp;
        if (!vectorize(c->rb + headers_end, content_len, vec)) {
            resp = &g_resp_400;
        } else {
            int frauds = g_knn(vec);
            resp = &g_resp_fraud[frauds];
        }
        c->wb = resp->data; c->wlen = resp->len; c->wp = 0;

        memmove(c->rb, c->rb + body_end, c->rp - body_end);
        c->rp -= body_end;
        return 1;
    }

    if (c->rp >= 11 && memcmp(c->rb, "GET /ready ", 11) == 0) {
        c->wb = g_resp_ready.data; c->wlen = g_resp_ready.len; c->wp = 0;
        memmove(c->rb, c->rb + headers_end, c->rp - headers_end);
        c->rp -= headers_end;
        return 1;
    }

    c->wb = g_resp_404.data; c->wlen = g_resp_404.len; c->wp = 0;
    memmove(c->rb, c->rb + headers_end, c->rp - headers_end);
    c->rp -= headers_end;
    return 1;
}


static void on_readable(Conn* c);

static void drain(Conn* c) {
    while (c->wb == nullptr) {
        int r = try_process_one(c);
        if (r == 0) return;
        if (r < 0) { conn_close(c); return; }
        try_send(c);
        if (c->wb != nullptr) return;
    }
}

static void on_readable(Conn* c) {
    if (c->wb != nullptr) {
        try_send(c);
        if (c->wb != nullptr) return;
    }
    while (true) {
        if (c->rp >= RBUF_SIZE) { conn_close(c); return; }
        ssize_t n = recv(c->fd, c->rb + c->rp, RBUF_SIZE - c->rp, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            conn_close(c);
            return;
        }
        if (n == 0) { conn_close(c); return; }
        c->rp += (size_t)n;
    }
    drain(c);
}

static void on_writable(Conn* c) {
    if (c->wb != nullptr) {
        try_send(c);
        if (c->wb != nullptr) return;
    }
    drain(c);
}


static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int listen_unix(const char* path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(addr.sun_path)) { fprintf(stderr, "socket path too long\n"); return -1; }
    memcpy(addr.sun_path, path, plen);

    umask(0);
    if (bind(fd, (sockaddr*)&addr, sizeof(sa_family_t) + plen) < 0) {
        perror("bind");
        return -1;
    }
    chmod(path, 0666);
    if (listen(fd, LISTEN_BACKLOG) < 0) { perror("listen"); return -1; }
    return fd;
}

static int listen_tcp(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return -1; }
    if (listen(fd, LISTEN_BACKLOG) < 0) { perror("listen"); return -1; }
    return fd;
}

static void accept_all(int listen_fd) {
    while (true) {
        int fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            return;
        }
        Conn* c = conn_alloc(fd);
        if (!c) { close(fd); continue; }
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = c;
        if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            close(fd);
            conn_free(c);
        }
    }
}


int main() {
    signal(SIGPIPE, SIG_IGN);

    const char* refs_path = getenv("REFS_PATH");
    const char* labels_path = getenv("LABELS_PATH");
    if (!refs_path)   refs_path   = DEFAULT_REFS_PATH;
    if (!labels_path) labels_path = DEFAULT_LABELS_PATH;

    g_refs   = (const int16_t*)mmap_file(refs_path,   N_REFS * VDIM_PADDED * sizeof(int16_t));
    g_labels = (const uint8_t*)mmap_file(labels_path, N_REFS);

    __builtin_cpu_init();
    const bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    g_knn = has_avx2 ? knn_avx2 : knn_scalar;
    fprintf(stdout, "KNN backend: %s\n", has_avx2 ? "avx2+fma" : "scalar");

    init_responses();
    pool_init();

    int listen_fd;
    const char* sock = getenv("SOCKET_PATH");
    if (sock && *sock) {
        listen_fd = listen_unix(sock);
        fprintf(stdout, "listening on unix:%s\n", sock);
    } else {
        listen_fd = listen_tcp(3000);
        fprintf(stdout, "listening on tcp:3000\n");
    }
    if (listen_fd < 0) return 1;
    fflush(stdout);

    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd < 0) { perror("epoll_create1"); return 1; }

    epoll_event lev{};
    lev.events = EPOLLIN | EPOLLET;
    lev.data.ptr = nullptr;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, listen_fd, &lev) < 0) {
        perror("epoll_ctl listen");
        return 1;
    }

    epoll_event evs[MAX_EVENTS];
    while (true) {
        int n = epoll_wait(g_epfd, evs, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            void* ptr = evs[i].data.ptr;
            uint32_t e = evs[i].events;
            if (ptr == nullptr) {
                accept_all(listen_fd);
                continue;
            }
            Conn* c = (Conn*)ptr;
            if (e & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                if (e & EPOLLIN) on_readable(c);
                if (c->in_use) conn_close(c);
                continue;
            }
            if (e & EPOLLIN)  on_readable(c);
            if (c->in_use && (e & EPOLLOUT)) on_writable(c);
        }
    }
    return 0;
}

