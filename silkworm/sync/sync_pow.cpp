/*
   Copyright 2022 The Silkworm Authors

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

#include "sync_pow.hpp"

#include <silkworm/core/common/as_range.hpp>
#include <silkworm/infra/common/measure.hpp>
#include <silkworm/infra/concurrency/sync_wait.hpp>
#include <silkworm/sync/messages/outbound_new_block.hpp>
#include <silkworm/sync/messages/outbound_new_block_hashes.hpp>

namespace silkworm::chainsync {

static void ensure_invariant(bool condition, std::string message) {
    if (!condition)
        throw std::logic_error("Consensus invariant violation: " + message);
}

PoWSync::PoWSync(BlockExchange& be, execution::Client& ee)
    : block_exchange_{be},
      exec_engine_{ee},
      chain_fork_view_{ChainHead{}} {  // we cannot call ee.get_canonical_head() at this point because ee is not started
}

auto PoWSync::resume() -> NewHeight {                                          // find the point (head) where we left off
    auto head = sync_wait(in(exec_engine_), exec_engine_.last_fork_choice());  // previously was get_canonical_head()
    auto block_progress = sync_wait(in(exec_engine_), exec_engine_.block_progress());

    chain_fork_view_.reset_head(head);

    ensure_invariant(height(head) <= block_progress, "canonical head beyond block progress");

    // if canonical and header progress match than canonical head was updated, we only need to do a forward sync...

    if (block_progress == height(head)) {
        return head;
    }

    // ... else we have to re-compute the canonical head parsing the last N headers

    auto prev_headers = sync_wait(in(exec_engine_), exec_engine_.get_last_headers(128));  // are 128 headers enough?
    as_range::for_each(prev_headers, [&, this](const auto& header) {
        chain_fork_view_.add(header);
    });

    return to_BlockId(chain_fork_view_.head());
}

auto PoWSync::forward_and_insert_blocks() -> NewHeight {
    using ResultQueue = BlockExchange::ResultQueue;

    ResultQueue& downloading_queue = block_exchange_.result_queue();

    auto initial_block_progress = sync_wait(in(exec_engine_), exec_engine_.block_progress());
    auto block_progress = initial_block_progress;

    block_exchange_.download_blocks(initial_block_progress, BlockExchange::Target_Tracking::kByAnnouncements);

    StopWatch timing(StopWatch::kStart);
    RepeatedMeasure<BlockNum> downloaded_headers(initial_block_progress);
    log::Info("Sync") << "Waiting for blocks... from=" << initial_block_progress;

    while (!is_stopping() &&
           !(block_exchange_.in_sync() && block_progress == block_exchange_.current_height())) {
        Blocks blocks;

        // wait for a batch of blocks
        bool present = downloading_queue.timed_wait_and_pop(blocks, 100ms);
        if (!present) continue;

        Blocks announcements_to_do;

        // compute head of chain applying fork choice rule
        as_range::for_each(blocks, [&, this](const auto& block) {
            block->td = chain_fork_view_.add(block->header);
            block_progress = std::max(block_progress, block->header.number);
            if (block->to_announce) announcements_to_do.push_back(block);
        });

        // insert blocks into database
        sync_wait(in(exec_engine_), exec_engine_.insert_blocks(to_plain_blocks(blocks)));

        // send announcement to peers
        send_new_block_announcements(std::move(announcements_to_do));  // according to eth/67 they must be done here,
                                                                       // after simple header verification

        downloaded_headers.set(block_progress);
        log::Info("Sync") << "Downloading progress: +" << downloaded_headers.delta() << " blocks downloaded, "
                          << downloaded_headers.high_res_throughput<seconds_t>() << " headers/secs"
                          << ", last=" << downloaded_headers.get()
                          << ", head=" << chain_fork_view_.head_height()
                          << ", lap.duration=" << StopWatch::format(timing.since_start());
    };

    block_exchange_.stop_downloading();

    auto [tp, duration] = timing.stop();
    log::Info("Sync") << "Downloading completed, last=" << block_progress
                      << ", head=" << chain_fork_view_.head_height()
                      << ", tot.duration=" << StopWatch::format(duration);

    return {.number = chain_fork_view_.head_height(), .hash = chain_fork_view_.head_hash()};
}

void PoWSync::unwind(UnwindPoint, std::optional<Hash>) {
    // does nothing
}

void PoWSync::execution_loop() {
    using namespace execution;
    bool is_starting_up = true;

    auto last_headers = sync_wait(in(exec_engine_), exec_engine_.get_last_headers(65536));
    block_exchange_.initial_state(last_headers);  // BlockExchange need a starting point to start downloading from

    while (!is_stopping()) {
        // resume from previous run or download new blocks
        NewHeight new_height = is_starting_up
                                   ? resume()                      // resuming, the following verify_chain is needed to check all stages
                                   : forward_and_insert_blocks();  // downloads new blocks and inserts them into the db
        if (new_height.number == 0) {
            // When starting from empty db there is no chain to verify, so go on downloading new blocks
            is_starting_up = false;
            continue;
        }

        // verify the new section of the chain
        log::Info("Sync") << "Verifying chain, head=" << new_height.number;
        auto verification = sync_wait(in(exec_engine_), exec_engine_.validate_chain(new_height.hash));  // BLOCKING

        if (std::holds_alternative<ValidChain>(verification)) {
            auto valid_chain = std::get<ValidChain>(verification);

            log::Info("Sync") << "Valid chain, new head=" << new_height.number;

            // if it is valid, do nothing, only check invariant
            ensure_invariant(valid_chain.current_head == new_height.hash, "Invalid verify_chain result");

            // notify fork choice update
            log::Info("Sync") << "Notifying fork choice updated, new head=" << new_height.number;
            sync_wait(in(exec_engine_), exec_engine_.update_fork_choice(new_height.hash));

            send_new_block_hash_announcements();  // according to eth/67 they must be done after a full block verification

        } else if (std::holds_alternative<InvalidChain>(verification)) {
            auto invalid_chain = std::get<InvalidChain>(verification);

            auto latest_valid_height = sync_wait(in(exec_engine_), exec_engine_.get_block_num(invalid_chain.latest_valid_head));
            ensure_invariant(latest_valid_height.has_value(), "Invalid latest_valid_head");

            log::Info("Sync") << "Invalid chain, unwinding down to=" << *latest_valid_height;

            // if it is not valid, unwind the chain
            unwind({*latest_valid_height, invalid_chain.latest_valid_head}, invalid_chain.bad_block);

            if (!invalid_chain.bad_headers.empty()) {
                update_bad_headers(std::move(invalid_chain.bad_headers));
            }

            // notify fork choice update
            log::Info("Sync") << "Notifying fork choice updated, head=" << to_hex(invalid_chain.latest_valid_head);
            sync_wait(in(exec_engine_), exec_engine_.update_fork_choice(invalid_chain.latest_valid_head));

        } else if (std::holds_alternative<ValidationError>(verification)) {
            // if it returned a validation error, raise an exception
            auto error = std::get<ValidationError>(verification);
            throw std::logic_error("Consensus, validation error, last point=" + to_hex(error.latest_valid_head) +
                                   ", missing block=" + to_hex(error.missing_block));

        } else {
            throw std::logic_error("Consensus, unknown error");
        }

        is_first_sync_ = is_starting_up;
        is_starting_up = false;
    }
};

auto PoWSync::update_bad_headers(std::set<Hash> bad_headers) -> std::shared_ptr<InternalMessage<void>> {
    auto message = std::make_shared<InternalMessage<void>>(
        [bads = std::move(bad_headers)](HeaderChain& hc, BodySequence&) { hc.add_bad_headers(bads); });

    block_exchange_.accept(message);

    return message;
}

// New block hash announcements propagation
void PoWSync::send_new_block_hash_announcements() {
    auto message = std::make_shared<OutboundNewBlockHashes>(is_first_sync_);

    block_exchange_.accept(message);
}

// New block announcements propagation
void PoWSync::send_new_block_announcements(Blocks&& blocks) {
    if (blocks.empty()) return;

    auto message = std::make_shared<OutboundNewBlock>(std::move(blocks), is_first_sync_);

    block_exchange_.accept(message);
}

}  // namespace silkworm::chainsync