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

#include "util/async/AnyStopToken.h"

#include <gtest/gtest.h>

using namespace util::async;
using namespace ::testing;

namespace {
struct FakeStopToken {
    bool isStopRequested_ = false;
    bool
    isStopRequested() const
    {
        return isStopRequested_;
    }
};
}  // namespace

TEST(AnyStopTokenTests, IsStopRequestedCallPropagated)
{
    {
        AnyStopToken stopToken{FakeStopToken{false}};
        EXPECT_EQ(stopToken.isStopRequested(), false);
    }
    {
        AnyStopToken stopToken{FakeStopToken{true}};
        EXPECT_EQ(stopToken.isStopRequested(), true);
    }
}

TEST(AnyStopTokenTests, CanCopy)
{
    AnyStopToken stopToken{FakeStopToken{true}};
    AnyStopToken token = stopToken;

    EXPECT_EQ(token, stopToken);
}