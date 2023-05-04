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

#include <backend/BackendInterface.h>
#include <rpc/RPCHelpers.h>
#include <rpc/common/Types.h>
#include <rpc/common/Validators.h>

namespace RPC {
class LedgerDataHandler
{
    // dependencies
    std::shared_ptr<BackendInterface> sharedPtrBackend_;
    clio::Logger log_{"RPC"};

    // constants
    static uint32_t constexpr LIMITBINARY = 2048;
    static uint32_t constexpr LIMITJSON = 256;

public:
    struct Output
    {
        uint32_t ledgerIndex;
        std::string ledgerHash;
        std::optional<boost::json::object> header;
        boost::json::array states;
        std::optional<std::string> marker;
        std::optional<uint32_t> diffMarker;
        std::optional<bool> cacheFull;
        bool validated = true;
    };

    // TODO: Clio does not implement "type" filter
    // outOfOrder only for clio, there is no document, traverse via seq diff
    // outOfOrder implementation is copied from old rpc handler
    struct Input
    {
        std::optional<std::string> ledgerHash;
        std::optional<uint32_t> ledgerIndex;
        bool binary = false;
        uint32_t limit = LedgerDataHandler::LIMITJSON;  // max 256 for json ; 2048 for binary
        std::optional<ripple::uint256> marker;
        std::optional<uint32_t> diffMarker;
        bool outOfOrder = false;
    };

    using Result = HandlerReturnType<Output>;

    LedgerDataHandler(std::shared_ptr<BackendInterface> const& sharedPtrBackend) : sharedPtrBackend_(sharedPtrBackend)
    {
    }

    RpcSpecConstRef
    spec() const
    {
        static const auto rpcSpec = RpcSpec{
            {JS(binary), validation::Type<bool>{}},
            {"out_of_order", validation::Type<bool>{}},
            {JS(ledger_hash), validation::Uint256HexStringValidator},
            {JS(ledger_index), validation::LedgerIndexValidator},
            {JS(limit), validation::Type<uint32_t>{}},
            {JS(marker),
             validation::Type<uint32_t, std::string>{},
             validation::IfType<std::string>{validation::Uint256HexStringValidator}},
        };
        return rpcSpec;
    }

    Result
    process(Input input, Context const& ctx) const;

private:
    friend void
    tag_invoke(boost::json::value_from_tag, boost::json::value& jv, Output const& output);

    friend Input
    tag_invoke(boost::json::value_to_tag<Input>, boost::json::value const& jv);
};
}  // namespace RPC