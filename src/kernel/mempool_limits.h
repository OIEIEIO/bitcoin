// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_KERNEL_MEMPOOL_LIMITS_H
#define BITCOIN_KERNEL_MEMPOOL_LIMITS_H

#include <policy/policy.h>

#include <cstdint>

namespace kernel {
/**
 * Options struct containing limit options for a CTxMemPool. Default constructor
 * populates the struct with sane default values which can be modified.
 *
 * Most of the time, this struct should be referenced as CTxMemPool::Limits.
 */
struct MemPoolLimits {
    //! The maximum number of transactions in a cluster.
    int64_t cluster_count{DEFAULT_CLUSTER_LIMIT};
    //! The maximum allowed size in virtual bytes of a cluster.
    int64_t cluster_size_vbytes{DEFAULT_CLUSTER_SIZE_LIMIT_KVB * 1'000};
    /**
     * @return MemPoolLimits with all the limits set to the maximum
     */
    static constexpr MemPoolLimits NoLimits()
    {
        int64_t no_limit{std::numeric_limits<int64_t>::max()};
        return {no_limit, no_limit};
    }
};
} // namespace kernel

#endif // BITCOIN_KERNEL_MEMPOOL_LIMITS_H
