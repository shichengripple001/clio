//------------------------------------------------------------------------------
/*
    This file is part of clio: https://github.com/XRPLF/clio
    Copyright (c) 2022, the clio developers.

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

#include <backend/BackendInterface.h>
#include <config/Config.h>
#include <etl/Source.h>
#include <log/Logger.h>
#include <rpc/Counters.h>
#include <rpc/Errors.h>
#include <rpc/RPCHelpers.h>
#include <rpc/common/AnyHandler.h>
#include <rpc/common/Types.h>
#include <rpc/common/impl/AdminVerificationStrategy.h>
#include <rpc/common/impl/ForwardingProxy.h>
#include <util/Taggable.h>
#include <webserver/Context.h>
#include <webserver/DOSGuard.h>

#include <boost/asio/spawn.hpp>
#include <boost/json.hpp>
#include <fmt/core.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

class SubscriptionManager;
class LoadBalancer;
class ETLService;

namespace RPC {

/**
 * @brief The RPC engine that ties all RPC-related functionality together
 */
template <typename AdminVerificationStrategyType>
class RPCEngineBase
{
    clio::Logger perfLog_{"Performance"};
    clio::Logger log_{"RPC"};

    std::shared_ptr<BackendInterface> backend_;
    std::shared_ptr<SubscriptionManager> subscriptions_;
    std::shared_ptr<LoadBalancer> balancer_;
    std::reference_wrapper<clio::DOSGuard const> dosGuard_;
    std::reference_wrapper<WorkQueue> workQueue_;
    std::reference_wrapper<Counters> counters_;

    std::shared_ptr<HandlerProvider const> handlerProvider_;

    detail::ForwardingProxy<LoadBalancer, Counters, HandlerProvider> forwardingProxy_;
    AdminVerificationStrategyType adminVerifier_;

public:
    RPCEngineBase(
        std::shared_ptr<BackendInterface> const& backend,
        std::shared_ptr<SubscriptionManager> const& subscriptions,
        std::shared_ptr<LoadBalancer> const& balancer,
        std::shared_ptr<ETLService> const& etl,
        clio::DOSGuard const& dosGuard,
        WorkQueue& workQueue,
        Counters& counters,
        std::shared_ptr<HandlerProvider const> const& handlerProvider)
        : backend_{backend}
        , subscriptions_{subscriptions}
        , balancer_{balancer}
        , dosGuard_{std::cref(dosGuard)}
        , workQueue_{std::ref(workQueue)}
        , counters_{std::ref(counters)}
        , handlerProvider_{handlerProvider}
        , forwardingProxy_{balancer, counters, handlerProvider}
    {
    }

    static std::shared_ptr<RPCEngineBase>
    make_RPCEngine(
        clio::Config const& config,
        std::shared_ptr<BackendInterface> const& backend,
        std::shared_ptr<SubscriptionManager> const& subscriptions,
        std::shared_ptr<LoadBalancer> const& balancer,
        std::shared_ptr<ETLService> const& etl,
        clio::DOSGuard const& dosGuard,
        WorkQueue& workQueue,
        Counters& counters,
        std::shared_ptr<HandlerProvider const> const& handlerProvider)
    {
        return std::make_shared<RPCEngineBase>(
            backend, subscriptions, balancer, etl, dosGuard, workQueue, counters, handlerProvider);
    }

    /**
     * @brief Main request processor routine
     * @param ctx The @ref Context of the request
     */
    Result
    buildResponse(Web::Context const& ctx)
    {
        if (forwardingProxy_.shouldForward(ctx))
            return forwardingProxy_.forward(ctx);

        if (backend_->isTooBusy())
        {
            log_.error() << "Database is too busy. Rejecting request";
            notifyTooBusy();  // TODO: should we add ctx.method if we have it?
            return Status{RippledError::rpcTOO_BUSY};
        }

        auto const method = handlerProvider_->getHandler(ctx.method);
        if (!method)
        {
            notifyUnknownCommand();
            return Status{RippledError::rpcUNKNOWN_COMMAND};
        }

        try
        {
            perfLog_.debug() << ctx.tag() << " start executing rpc `" << ctx.method << '`';

            auto const isAdmin = adminVerifier_.isAdmin(ctx.clientIp);
            auto const context = Context{ctx.yield, ctx.session, isAdmin, ctx.clientIp, ctx.apiVersion};
            auto const v = (*method).process(ctx.params, context);

            perfLog_.debug() << ctx.tag() << " finish executing rpc `" << ctx.method << '`';

            if (v)
                return v->as_object();
            else
            {
                notifyErrored(ctx.method);
                return Status{v.error()};
            }
        }
        catch (Backend::DatabaseTimeout const& t)
        {
            log_.error() << "Database timeout";
            notifyTooBusy();

            return Status{RippledError::rpcTOO_BUSY};
        }
        catch (std::exception const& ex)
        {
            log_.error() << ctx.tag() << "Caught exception: " << ex.what();
            notifyInternalError();

            return Status{RippledError::rpcINTERNAL};
        }
    }

    /**
     * @brief Used to schedule request processing onto the work queue
     * @param func The lambda to execute when this request is handled
     * @param ip The ip address for which this request is being executed
     */
    template <typename Fn>
    bool
    post(Fn&& func, std::string const& ip)
    {
        return workQueue_.get().postCoro(std::forward<Fn>(func), dosGuard_.get().isWhiteListed(ip));
    }

    /**
     * @brief Notify the system that specified method was executed
     * @param method
     * @param duration The time it took to execute the method specified in
     * microseconds
     */
    void
    notifyComplete(std::string const& method, std::chrono::microseconds const& duration)
    {
        if (validHandler(method))
            counters_.get().rpcComplete(method, duration);
    }

    /**
     * @brief Notify the system that specified method failed to execute due to a recoverable user error
     *
     * Used for errors based on user input, not actual failures of the db or clio itself.
     *
     * @param method
     */
    void
    notifyFailed(std::string const& method)
    {
        // FIXME: seems like this is not used?
        if (validHandler(method))
            counters_.get().rpcFailed(method);
    }

    /**
     * @brief Notify the system that specified method failed due to some unrecoverable error
     *
     * Used for erors such as database timeout, internal errors, etc.
     *
     * @param method
     */
    void
    notifyErrored(std::string const& method)
    {
        if (validHandler(method))
            counters_.get().rpcErrored(method);
    }

    /**
     * @brief Notify the system that the RPC system is too busy to handle an incoming request
     */
    void
    notifyTooBusy()
    {
        counters_.get().onTooBusy();
    }

    /**
     * @brief Notify the system that the RPC system was not ready to handle an incoming request
     *
     * This happens when the backend is not yet have a ledger range
     */
    void
    notifyNotReady()
    {
        counters_.get().onNotReady();
    }

    /**
     * @brief Notify the system that the incoming request did not specify the RPC method/command
     */
    void
    notifyBadSyntax()
    {
        counters_.get().onBadSyntax();
    }

    /**
     * @brief Notify the system that the incoming request specified an unknown/unsupported method/command
     */
    void
    notifyUnknownCommand()
    {
        counters_.get().onUnknownCommand();
    }

    /**
     * @brief Notify the system that the incoming request lead to an internal error (unrecoverable)
     */
    void
    notifyInternalError()
    {
        counters_.get().onInternalError();
    }

private:
    bool
    validHandler(std::string const& method) const
    {
        return handlerProvider_->contains(method) || forwardingProxy_.isProxied(method);
    }
};

using RPCEngine = RPCEngineBase<detail::IPAdminVerificationStrategy>;

}  // namespace RPC