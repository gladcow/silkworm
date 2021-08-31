/*
   Copyright 2021 The Silkworm Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#ifndef SILKWORM_BLOCK_PROVIDER_HPP
#define SILKWORM_BLOCK_PROVIDER_HPP

#include <chrono>

#include <silkworm/chain/identity.hpp>
#include <silkworm/concurrency/active_component.hpp>
#include <silkworm/concurrency/containers.hpp>

#include "messages/Message.hpp"
#include "sentry_client.hpp"
#include "silkworm/downloader/internals/DbTx.hpp"
#include "silkworm/downloader/internals/singleton.hpp"
#include "silkworm/downloader/internals/types.hpp"

namespace silkworm {

class BlockProvider : public ActiveComponent {  // but also an active component that must run always

    ChainIdentity chain_identity_;
    DbTx db_;
    ActiveSentryClient& sentry_;

  public:
    BlockProvider(ActiveSentryClient& sentry, ChainIdentity chain_identity, std::string db_path);
    BlockProvider(const BlockProvider&) = delete;  // not copyable
    BlockProvider(BlockProvider&&) = delete;       // nor movable
    ~BlockProvider();

    DbTx& db_tx() { return db_; }
    SentryClient& sentry() { return sentry_; }

    void execution_loop() override;

  private:
    using MessageQueue = ConcurrentQueue<std::shared_ptr<Message>>;

    void send_status();
    void send_message_subscription(MessageQueue& messages);

    void process_one_message(MessageQueue& messages);
};

}  // namespace silkworm

#endif  // SILKWORM_BLOCK_PROVIDER_HPP