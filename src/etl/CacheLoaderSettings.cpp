//------------------------------------------------------------------------------
/*
    This file is part of clio: https://github.com/XRPLF/clio
    Copyright (c) 2024, the clio developers.

    Permission to use, copy, modify, and distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL,  DIRECT,  INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include "etl/CacheLoaderSettings.hpp"

#include "util/config/Config.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <ripple/basics/Blob.h>
#include <ripple/basics/base_uint.h>
#include <ripple/basics/strHex.h>

#include <cstddef>
#include <string>

namespace etl {

[[nodiscard]] bool
CacheLoaderSettings::isSync() const
{
    return loadStyle == LoadStyle::SYNC;
}

[[nodiscard]] bool
CacheLoaderSettings::isAsync() const
{
    return loadStyle == LoadStyle::ASYNC;
}

[[nodiscard]] bool
CacheLoaderSettings::isDisabled() const
{
    return loadStyle == LoadStyle::NOT_AT_ALL;
}

[[nodiscard]] CacheLoaderSettings
make_CacheLoaderSettings(util::Config const& config)
{
    CacheLoaderSettings settings;
    settings.numThreads = config.valueOr("io_threads", settings.numThreads);
    if (config.contains("cache")) {
        auto const cache = config.section("cache");
        settings.numCacheDiffs = cache.valueOr<size_t>("num_diffs", settings.numCacheDiffs);
        settings.numCacheMarkers = cache.valueOr<size_t>("num_markers", settings.numCacheMarkers);
        settings.cachePageFetchSize = cache.valueOr<size_t>("page_fetch_size", settings.cachePageFetchSize);

        if (auto entry = cache.maybeValue<std::string>("load"); entry) {
            if (boost::iequals(*entry, "sync"))
                settings.loadStyle = CacheLoaderSettings::LoadStyle::SYNC;
            if (boost::iequals(*entry, "async"))
                settings.loadStyle = CacheLoaderSettings::LoadStyle::ASYNC;
            if (boost::iequals(*entry, "none") or boost::iequals(*entry, "no"))
                settings.loadStyle = CacheLoaderSettings::LoadStyle::NOT_AT_ALL;
        }
    }
    return settings;
}

}  // namespace etl