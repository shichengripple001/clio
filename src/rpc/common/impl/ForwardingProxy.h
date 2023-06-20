//------------------------------------------------------------------------------
/*
    This file is part of clio: https://github.com/XRPLF/clio
    Copyright (c) 2023, the clio developers.

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

#pragma once

#include <log/Logger.h>
#include <rpc/common/Types.h>
#include <webserver/Context.h>

#include <memory>
#include <string>

namespace RPC::detail {

template <typename CountersType>
class ForwardingProxy
{
    clio::Logger log_{"RPC"};

    std::shared_ptr<LoadBalancer> balancer_;
    std::reference_wrapper<CountersType> counters_;
    std::shared_ptr<HandlerProvider const> handlerProvider_;

public:
    ForwardingProxy(
        std::shared_ptr<LoadBalancer> const& balancer,
        CountersType& counters,
        std::shared_ptr<HandlerProvider const> const& handlerProvider)
        : balancer_{balancer}, counters_{std::ref(counters)}, handlerProvider_{handlerProvider}
    {
    }

    bool
    shouldForward(Web::Context const& ctx) const;

    Result
    forward(Web::Context const& ctx);

    bool
    isProxied(std::string const& method) const;

private:
    void
    notifyForwarded(std::string const& method);

    void
    notifyFailedToForward(std::string const& method);

    bool
    validHandler(std::string const& method) const;
};

}  // namespace RPC::detail
