// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <random.h>

#include <crypto/sha512.h>
#include <support/cleanse.h>
#ifdef WIN32
#include <compat.h> // for Windows API
#include <wincrypt.h>
#endif
#include <util.h>             // for LogPrint()
#include <utilstrencodings.h> // for GetTime()

#include <stdlib.h>
#include <limits>
#include <chrono>
#include <thread>

#ifndef WIN32
#include <sys/time.h>
#endif

#ifdef HAVE_SYS_GETRANDOM
#include <sys/syscall.h>
#include <linux/random.h>
#endif
#if defined(HAVE_GETENTROPY) || (defined(HAVE_GETENTROPY_RAND) && defined(MAC_OSX))
#include <unistd.h>
#endif
#if defined(HAVE_GETENTROPY_RAND) && defined(MAC_OSX)
#include <sys/random.h>
#endif
#ifdef HAVE_SYSCTL_ARND
#include <sys/sysctl.h>
#endif

#include <mutex>

#if defined(__x86_64__) || defined(__amd64__) || defined(__i386__)
#include <cpuid.h>
#endif

[[noreturn]] static void RandFailure()
{
    LogPrintf("Failed to read randomness, aborting\n");
    std::abort();
}

static inline int64_t GetPerformanceCounter()
{
    // Read the hardware time stamp counter when available.
    // See https://en.wikipedia.org/wiki/Time_Stamp_Counter for more information.
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    return __rdtsc();
#elif !defined(_MSC_VER) && defined(__i386__)
    uint64_t r = 0;
    __asm__ volatile ("rdtsc" : "=A"(r)); // Constrain the r variable to the eax:edx pair.
    return r;
#elif !defined(_MSC_VER) && (defined(__x86_64__) || defined(__amd64__))
    uint64_t r1 = 0, r2 = 0;
    __asm__ volatile ("rdtsc" : "=a"(r1), "=d"(r2)); // Constrain r1 to rax and r2 to rdx.
    return (r2 << 32) | r1;
#else
    // Fall back to using C++11 clock (usually microsecond or nanosecond precision)
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
#endif
}


#if defined(__x86_64__) || defined(__amd64__) || defined(__i386__)
static std::atomic<bool> hwrand_initialized{false};
static bool rdrand_supported = false;
static constexpr uint32_t CPUID_F1_ECX_RDRAND = 0x40000000;
static void RDRandInit()
{
    uint32_t eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & CPUID_F1_ECX_RDRAND)) {
        LogPrintf("Using RdRand as an additional entropy source\n");
        rdrand_supported = true;
    }
    hwrand_initialized.store(true);
}
#else
static void RDRandInit() {}
#endif

static bool GetHWRand(unsigned char* ent32) {
#if defined(__x86_64__) || defined(__amd64__) || defined(__i386__)
    assert(hwrand_initialized.load(std::memory_order_relaxed));
    if (rdrand_supported) {
        uint8_t ok;
        // Not all assemblers support the rdrand instruction, write it in hex.
#ifdef __i386__
        for (int iter = 0; iter < 4; ++iter) {
            uint32_t r1, r2;
            __asm__ volatile (".byte 0x0f, 0xc7, 0xf0;" // rdrand %eax
                              ".byte 0x0f, 0xc7, 0xf2;" // rdrand %edx
                              "setc %2" :
                              "=a"(r1), "=d"(r2), "=q"(ok) :: "cc");
            if (!ok) return false;
            WriteLE32(ent32 + 8 * iter, r1);
            WriteLE32(ent32 + 8 * iter + 4, r2);
        }
#else
        uint64_t r1, r2, r3, r4;
        __asm__ volatile (".byte 0x48, 0x0f, 0xc7, 0xf0, " // rdrand %rax
                                "0x48, 0x0f, 0xc7, 0xf3, " // rdrand %rbx
                                "0x48, 0x0f, 0xc7, 0xf1, " // rdrand %rcx
                                "0x48, 0x0f, 0xc7, 0xf2; " // rdrand %rdx
                          "setc %4" :
                          "=a"(r1), "=b"(r2), "=c"(r3), "=d"(r4), "=q"(ok) :: "cc");
        if (!ok) return false;
        WriteLE64(ent32, r1);
        WriteLE64(ent32 + 8, r2);
        WriteLE64(ent32 + 16, r3);
        WriteLE64(ent32 + 24, r4);
#endif
        return true;
    }
#endif
    return false;
}

#ifndef WIN32
/** Fallback: get 32 bytes of system entropy from /dev/urandom. The most
 * compatible way to get cryptographic randomness on UNIX-ish platforms.
 */
void GetDevURandom(unsigned char *ent32)
{
    int f = open("/dev/urandom", O_RDONLY);
    if (f == -1) {
        RandFailure();
    }
    int have = 0;
    do {
        ssize_t n = read(f, ent32 + have, NUM_OS_RANDOM_BYTES - have);
        if (n <= 0 || n + have > NUM_OS_RANDOM_BYTES) {
            close(f);
            RandFailure();
        }
        have += n;
    } while (have < NUM_OS_RANDOM_BYTES);
    close(f);
}
#endif

/** Get 32 bytes of system entropy. */
void GetOSRand(unsigned char *ent32)
{
#if defined(WIN32)
    HCRYPTPROV hProvider;
    int ret = CryptAcquireContextW(&hProvider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    if (!ret) {
        RandFailure();
    }
    ret = CryptGenRandom(hProvider, NUM_OS_RANDOM_BYTES, ent32);
    if (!ret) {
        RandFailure();
    }
    CryptReleaseContext(hProvider, 0);
#elif defined(HAVE_SYS_GETRANDOM)
    /* Linux. From the getrandom(2) man page:
     * "If the urandom source has been initialized, reads of up to 256 bytes
     * will always return as many bytes as requested and will not be
     * interrupted by signals."
     */
    int rv = syscall(SYS_getrandom, ent32, NUM_OS_RANDOM_BYTES, 0);
    if (rv != NUM_OS_RANDOM_BYTES) {
        if (rv < 0 && errno == ENOSYS) {
            /* Fallback for kernel <3.17: the return value will be -1 and errno
             * ENOSYS if the syscall is not available, in that case fall back
             * to /dev/urandom.
             */
            GetDevURandom(ent32);
        } else {
            RandFailure();
        }
    }
#elif defined(HAVE_GETENTROPY) && defined(__OpenBSD__)
    /* On OpenBSD this can return up to 256 bytes of entropy, will return an
     * error if more are requested.
     * The call cannot return less than the requested number of bytes.
       getentropy is explicitly limited to openbsd here, as a similar (but not
       the same) function may exist on other platforms via glibc.
     */
    if (getentropy(ent32, NUM_OS_RANDOM_BYTES) != 0) {
        RandFailure();
    }
#elif defined(HAVE_GETENTROPY_RAND) && defined(MAC_OSX)
    // We need a fallback for OSX < 10.12
    if (&getentropy != nullptr) {
        if (getentropy(ent32, NUM_OS_RANDOM_BYTES) != 0) {
            RandFailure();
        }
    } else {
        GetDevURandom(ent32);
    }
#elif defined(HAVE_SYSCTL_ARND)
    /* FreeBSD and similar. It is possible for the call to return less
     * bytes than requested, so need to read in a loop.
     */
    static const int name[2] = {CTL_KERN, KERN_ARND};
    int have = 0;
    do {
        size_t len = NUM_OS_RANDOM_BYTES - have;
        if (sysctl(name, ARRAYLEN(name), ent32 + have, &len, nullptr, 0) != 0) {
            RandFailure();
        }
        have += len;
    } while (have < NUM_OS_RANDOM_BYTES);
#else
    /* Fall back to /dev/urandom if there is no specific method implemented to
     * get system entropy for this OS.
     */
    GetDevURandom(ent32);
#endif
}

static unsigned char entropy[32] = {0};
static std::mutex cs_entropy;

void GetRandBytes(unsigned char* buf, int num)
{
    unsigned char seed32[32];

    // Entropy sources
    int64_t counter = GetPerformanceCounter(); // Time
    int64_t* pcounter = &counter; // Stack frame pointer (thread specific)
    unsigned char os_entropy[NUM_OS_RANDOM_BYTES]; // OS entropy
    GetOSRand(os_entropy);

    // Process entropy
    CSHA512 hasher;
    hasher.Write(os_entropy, NUM_OS_RANDOM_BYTES);
    memory_cleanse(os_entropy, NUM_OS_RANDOM_BYTES);
    hasher.Write((const unsigned char*)&counter, sizeof(counter));
    hasher.Write((const unsigned char*)&pcounter, sizeof(pcounter));

    {
        // Mix in global entropy, and update it.
        std::unique_lock<std::mutex> lock(cs_entropy);
        hasher.Write(entropy, 32);
        unsigned char out[64];
        hasher.Finalize(out);
        memcpy(seed32, out, 32);
        memcpy(entropy, out + 32, 32);
        memory_cleanse(out, 64);
    }

    // Produce output
    if (num <= 32) {
        memcpy(buf, seed32, num);
    } else {
       ChaCha20 prng(seed32, 32);
       prng.Output(buf, num);
    }
    memory_cleanse(seed32, 32);
}

static void AddDataToRng(void* data, size_t len);

void RandAddSeedSleep()
{
    int64_t nPerfCounter1 = GetPerformanceCounter();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    int64_t nPerfCounter2 = GetPerformanceCounter();

    // Combine with and update state
    AddDataToRng(&nPerfCounter1, sizeof(nPerfCounter1));
    AddDataToRng(&nPerfCounter2, sizeof(nPerfCounter2));

    memory_cleanse(&nPerfCounter1, sizeof(nPerfCounter1));
    memory_cleanse(&nPerfCounter2, sizeof(nPerfCounter2));
}


static std::mutex cs_rng_state;
static unsigned char rng_state[32] = {0};
static uint64_t rng_counter = 0;

static void AddDataToRng(void* data, size_t len) {
    CSHA512 hasher;
    hasher.Write((const unsigned char*)&len, sizeof(len));
    hasher.Write((const unsigned char*)data, len);
    unsigned char buf[64];
    {
        std::unique_lock<std::mutex> lock(cs_rng_state);
        hasher.Write(rng_state, sizeof(rng_state));
        hasher.Write((const unsigned char*)&rng_counter, sizeof(rng_counter));
        ++rng_counter;
        hasher.Finalize(buf);
        memcpy(rng_state, buf + 32, 32);
    }
    memory_cleanse(buf, 64);
}

uint64_t GetRand(uint64_t nMax)
{
    if (nMax == 0)
        return 0;

    // The range of the random source must be a multiple of the modulus
    // to give every possible output value an equal possibility
    uint64_t nRange = (std::numeric_limits<uint64_t>::max() / nMax) * nMax;
    uint64_t nRand = 0;
    do {
        GetRandBytes((unsigned char*)&nRand, sizeof(nRand));
    } while (nRand >= nRange);
    return (nRand % nMax);
}

int GetRandInt(int nMax)
{
    return GetRand(nMax);
}

uint256 GetRandHash()
{
    uint256 hash;
    GetRandBytes((unsigned char*)&hash, sizeof(hash));
    return hash;
}

void FastRandomContext::RandomSeed()
{
    uint256 seed = GetRandHash();
    rng.SetKey(seed.begin(), 32);
    requires_seed = false;
}

uint256 FastRandomContext::rand256()
{
    if (bytebuf_size < 32) {
        FillByteBuffer();
    }
    uint256 ret;
    memcpy(ret.begin(), bytebuf + 64 - bytebuf_size, 32);
    bytebuf_size -= 32;
    return ret;
}

std::vector<unsigned char> FastRandomContext::randbytes(size_t len)
{
    std::vector<unsigned char> ret(len);
    if (len > 0) {
        rng.Output(&ret[0], len);
    }
    return ret;
}

FastRandomContext::FastRandomContext(const uint256& seed) : requires_seed(false), bytebuf_size(0), bitbuf_size(0)
{
    rng.SetKey(seed.begin(), 32);
}

bool Random_SanityCheck()
{
    /* This does not measure the quality of randomness, but it does test that
     * OSRandom() overwrites all 32 bytes of the output given a maximum
     * number of tries.
     */
    static const ssize_t MAX_TRIES = 1024;
    uint8_t data[NUM_OS_RANDOM_BYTES];
    bool overwritten[NUM_OS_RANDOM_BYTES] = {}; /* Tracks which bytes have been overwritten at least once */
    int num_overwritten;
    int tries = 0;
    /* Loop until all bytes have been overwritten at least once, or max number tries reached */
    do {
        memset(data, 0, NUM_OS_RANDOM_BYTES);
        GetOSRand(data);
        for (int x=0; x < NUM_OS_RANDOM_BYTES; ++x) {
            overwritten[x] |= (data[x] != 0);
        }

        num_overwritten = 0;
        for (int x=0; x < NUM_OS_RANDOM_BYTES; ++x) {
            if (overwritten[x]) {
                num_overwritten += 1;
            }
        }

        tries += 1;
    } while (num_overwritten < NUM_OS_RANDOM_BYTES && tries < MAX_TRIES);
    return (num_overwritten == NUM_OS_RANDOM_BYTES); /* If this failed, bailed out after too many tries */
}

FastRandomContext::FastRandomContext(bool fDeterministic) : requires_seed(!fDeterministic), bytebuf_size(0), bitbuf_size(0)
{
    if (!fDeterministic) {
        return;
    }
    uint256 seed;
    rng.SetKey(seed.begin(), 32);
}

void RandomInit()
{
    RDRandInit();
}
