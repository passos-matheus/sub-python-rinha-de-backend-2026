#include "httplib.h"
#include "rapidjson/document.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <ctime>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <immintrin.h>


constexpr float MAX_AMOUNT              = 10000.0f;
constexpr float MAX_INSTALLMENTS        = 12.0f;
constexpr float AMOUNT_VS_AVG_RATIO     = 10.0f;
constexpr float MAX_MINUTES             = 1440.0f;
constexpr float MAX_KM                  = 1000.0f;
constexpr float MAX_TX_COUNT_24H        = 20.0f;
constexpr float MAX_MERCHANT_AVG_AMOUNT = 10000.0f;

constexpr int    VDIM        = 14;
constexpr int    VDIM_PADDED = 16;
constexpr int    KNN_K       = 5;
constexpr size_t N_REFS      = 100000;

constexpr int    PORT             = 3000;
constexpr size_t THREAD_POOL_SIZE = 4;

constexpr const char* REFS_PATH   = "/refs.bin";
constexpr const char* LABELS_PATH = "/labels.bin";


static const float*   g_refs   = nullptr;
static const uint8_t* g_labels = nullptr;


static const std::string RESPONSES[6] = {
    R"({"approved":true,"fraud_score":0})",
    R"({"approved":true,"fraud_score":0.2})",
    R"({"approved":true,"fraud_score":0.4})",
    R"({"approved":false,"fraud_score":0.6})",
    R"({"approved":false,"fraud_score":0.8})",
    R"({"approved":false,"fraud_score":1})",
};


static inline float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static inline float round4(float x) {
    return roundf(x * 10000.0f) / 10000.0f;
}

static inline int parse_int2(const char* s) {
    return (s[0] - '0') * 10 + (s[1] - '0');
}

static inline float mcc_risk(const char* mcc) {
    if (!mcc || !mcc[0] || !mcc[1] || !mcc[2] || !mcc[3]) return 0.5f;
    uint32_t code = (uint32_t(uint8_t(mcc[0])) << 24)
                  | (uint32_t(uint8_t(mcc[1])) << 16)
                  | (uint32_t(uint8_t(mcc[2])) << 8)
                  |  uint32_t(uint8_t(mcc[3]));
    switch (code) {
        case 0x35343131: return 0.15f;
        case 0x35383132: return 0.30f;
        case 0x35393132: return 0.20f;
        case 0x35393434: return 0.45f;
        case 0x37383031: return 0.80f;
        case 0x37383032: return 0.75f;
        case 0x37393935: return 0.85f;
        case 0x34353131: return 0.35f;
        case 0x35333131: return 0.25f;
        case 0x35393939: return 0.50f;
        default:         return 0.50f;
    }
}

static int64_t parse_iso8601(const char* ts) {
    struct tm t = {};
    t.tm_year = (ts[0]-'0')*1000 + (ts[1]-'0')*100 + (ts[2]-'0')*10 + (ts[3]-'0') - 1900;
    t.tm_mon  = parse_int2(ts + 5) - 1;
    t.tm_mday = parse_int2(ts + 8);
    t.tm_hour = parse_int2(ts + 11);
    t.tm_min  = parse_int2(ts + 14);
    t.tm_sec  = parse_int2(ts + 17);
    return (int64_t)timegm(&t);
}

static int day_of_week_iso(const char* ts) {
    int y = (ts[0]-'0')*1000 + (ts[1]-'0')*100 + (ts[2]-'0')*10 + (ts[3]-'0');
    int m = parse_int2(ts + 5);
    int d = parse_int2(ts + 8);
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100;
    int J = y / 100;
    int h = (d + 13*(m+1)/5 + K + K/4 + J/4 + 5*J) % 7;
    static const int iso_table[7] = {5, 6, 0, 1, 2, 3, 4};
    return iso_table[h];
}


static bool vectorize(const char* json_str, size_t len, float* out) {
    rapidjson::Document doc;
    doc.Parse(json_str, len);
    if (!doc.IsObject()) return false;

    const auto& tx    = doc["transaction"];
    const auto& cust  = doc["customer"];
    const auto& merch = doc["merchant"];
    const auto& term  = doc["terminal"];

    const float amount      = tx["amount"].GetFloat();
    const int installments  = tx["installments"].GetInt();
    const char* req_at      = tx["requested_at"].GetString();
    const float cust_avg    = cust["avg_amount"].GetFloat();
    const int tx_count_24h  = cust["tx_count_24h"].GetInt();
    const char* merch_id    = merch["id"].GetString();
    const char* mcc         = merch["mcc"].GetString();
    const float merch_avg   = merch["avg_amount"].GetFloat();
    const bool is_online    = term["is_online"].GetBool();
    const bool card_present = term["card_present"].GetBool();
    const float km_home     = term["km_from_home"].GetFloat();

    out[0] = clamp01(amount / MAX_AMOUNT);
    out[1] = clamp01((float)installments / MAX_INSTALLMENTS);
    out[2] = cust_avg > 0.0f ? clamp01((amount / cust_avg) / AMOUNT_VS_AVG_RATIO) : 0.0f;
    out[3] = (float)parse_int2(req_at + 11) / 23.0f;
    out[4] = (float)day_of_week_iso(req_at) / 6.0f;

    if (doc.HasMember("last_transaction") && !doc["last_transaction"].IsNull()) {
        const auto& last = doc["last_transaction"];
        int64_t now_t  = parse_iso8601(req_at);
        int64_t last_t = parse_iso8601(last["timestamp"].GetString());
        float diff_min = std::abs((float)(now_t - last_t)) / 60.0f;
        out[5] = clamp01(diff_min / MAX_MINUTES);
        out[6] = clamp01(last["km_from_current"].GetFloat() / MAX_KM);
    } else {
        out[5] = -1.0f;
        out[6] = -1.0f;
    }

    out[7]  = clamp01(km_home / MAX_KM);
    out[8]  = clamp01((float)tx_count_24h / MAX_TX_COUNT_24H);
    out[9]  = is_online ? 1.0f : 0.0f;
    out[10] = card_present ? 1.0f : 0.0f;

    bool known = false;
    for (auto& m : cust["known_merchants"].GetArray()) {
        if (strcmp(m.GetString(), merch_id) == 0) { known = true; break; }
    }
    out[11] = known ? 0.0f : 1.0f;
    out[12] = mcc_risk(mcc);
    out[13] = clamp01(merch_avg / MAX_MERCHANT_AVG_AMOUNT);

    for (int i = 0; i < VDIM; i++) out[i] = round4(out[i]);
    return true;
}


__attribute__((target("avx2,fma")))
static int knn_avx2(const float* vec) {
    alignas(32) float q[VDIM_PADDED] = {0};
    memcpy(q, vec, VDIM * sizeof(float));

    const __m256 q0 = _mm256_load_ps(q + 0);
    const __m256 q1 = _mm256_load_ps(q + 8);

    float   top_d[KNN_K];
    uint8_t top_l[KNN_K];
    for (int i = 0; i < KNN_K; i++) { top_d[i] = INFINITY; top_l[i] = 0; }

    for (size_t i = 0; i < N_REFS; i++) {
        const float* r = g_refs + i * VDIM_PADDED;
        __m256 r0 = _mm256_load_ps(r + 0);
        __m256 r1 = _mm256_load_ps(r + 8);
        __m256 d0 = _mm256_sub_ps(q0, r0);
        __m256 d1 = _mm256_sub_ps(q1, r1);
        __m256 sq = _mm256_fmadd_ps(d1, d1, _mm256_mul_ps(d0, d0));

        __m128 lo = _mm256_castps256_ps128(sq);
        __m128 hi = _mm256_extractf128_ps(sq, 1);
        __m128 s  = _mm_add_ps(lo, hi);
        s = _mm_hadd_ps(s, s);
        s = _mm_hadd_ps(s, s);
        const float d = _mm_cvtss_f32(s);

        if (d < top_d[KNN_K - 1]) {
            int j = KNN_K - 1;
            while (j > 0 && top_d[j - 1] > d) {
                top_d[j] = top_d[j - 1];
                top_l[j] = top_l[j - 1];
                j--;
            }
            top_d[j] = d;
            top_l[j] = g_labels[i];
        }
    }

    int fraud_n = 0;
    for (int i = 0; i < KNN_K; i++) fraud_n += top_l[i];
    return fraud_n;
}

static int knn_scalar(const float* vec) {
    alignas(32) float q[VDIM_PADDED] = {0};
    memcpy(q, vec, VDIM * sizeof(float));

    float   top_d[KNN_K];
    uint8_t top_l[KNN_K];
    for (int i = 0; i < KNN_K; i++) { top_d[i] = INFINITY; top_l[i] = 0; }

    for (size_t i = 0; i < N_REFS; i++) {
        const float* r = g_refs + i * VDIM_PADDED;
        float d = 0.0f;
        for (int k = 0; k < VDIM_PADDED; k++) {
            float diff = q[k] - r[k];
            d += diff * diff;
        }
        if (d < top_d[KNN_K - 1]) {
            int j = KNN_K - 1;
            while (j > 0 && top_d[j - 1] > d) {
                top_d[j] = top_d[j - 1];
                top_l[j] = top_l[j - 1];
                j--;
            }
            top_d[j] = d;
            top_l[j] = g_labels[i];
        }
    }

    int fraud_n = 0;
    for (int i = 0; i < KNN_K; i++) fraud_n += top_l[i];
    return fraud_n;
}

typedef int (*knn_fn)(const float*);
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


int main() {
    g_refs   = (const float*)  mmap_file(REFS_PATH,   N_REFS * VDIM_PADDED * sizeof(float));
    g_labels = (const uint8_t*)mmap_file(LABELS_PATH, N_REFS);

    __builtin_cpu_init();
    const bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    g_knn = has_avx2 ? knn_avx2 : knn_scalar;
    printf("KNN backend: %s\n", has_avx2 ? "avx2+fma" : "scalar");

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(THREAD_POOL_SIZE); };

    svr.Get("/ready", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
    });

    svr.Post("/fraud-score", [](const httplib::Request& req, httplib::Response& res) {
        float vec[VDIM];
        if (!vectorize(req.body.data(), req.body.size(), vec)) {
            res.status = 400;
            return;
        }
        const int fraud_n = g_knn(vec);
        res.set_content(RESPONSES[fraud_n], "application/json");
    });

    svr.set_tcp_nodelay(true);
    svr.set_keep_alive_max_count(10000);
    svr.set_keep_alive_timeout(30);

    const char* sock_path = getenv("SOCKET_PATH");
    if (sock_path && *sock_path) {
        unlink(sock_path);
        umask(0);
        svr.set_address_family(AF_UNIX);
        printf("Listening on unix:%s\n", sock_path);
        fflush(stdout);
        if (!svr.listen(sock_path, 1)) {
            fprintf(stderr, "listen failed on %s: %s\n", sock_path, strerror(errno));
            return 1;
        }
    } else {
        printf("Listening on port %d\n", PORT);
        fflush(stdout);
        svr.listen("0.0.0.0", PORT);
    }
    return 0;
}
