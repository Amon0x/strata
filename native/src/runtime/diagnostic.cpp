#include "runtime/diagnostic.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace strata::runtime {

RuntimeDiagnosticStore::RuntimeDiagnosticStore(const std::size_t max_records)
    : max_records_(max_records) {
    if (max_records_ == 0U) {
        throw std::invalid_argument("runtime diagnostic store capacity must be positive");
    }
}

const RuntimeDiagnosticRecord& RuntimeDiagnosticStore::append(
    const std::uint64_t frame_index,
    RuntimeDiagnostic diagnostic
) {
    const auto repeated = std::find_if(records_.rbegin(), records_.rend(), [&](const auto& record) {
        return frame_index >= record.frame_index &&
               frame_index - record.frame_index <= aggregation_frame_window &&
               record.diagnostic == diagnostic;
    });
    if (repeated != records_.rend()) {
        RuntimeDiagnosticRecord record = std::move(*repeated);
        records_.erase(std::next(repeated).base());
        record.frame_index = frame_index;
        if (record.occurrence_count != std::numeric_limits<std::size_t>::max()) {
            ++record.occurrence_count;
        }
        records_.push_back(std::move(record));
        return records_.back();
    }

    if (records_.size() == max_records_) {
        records_.pop_front();
        if (dropped_count_ != std::numeric_limits<std::uint64_t>::max()) ++dropped_count_;
    }
    records_.push_back(RuntimeDiagnosticRecord{
        next_sequence_, frame_index, frame_index, 1U, std::move(diagnostic),
    });
    if (next_sequence_ != std::numeric_limits<std::uint64_t>::max()) ++next_sequence_;
    return records_.back();
}

RuntimeDiagnosticsSnapshot RuntimeDiagnosticStore::snapshot(
    const std::uint64_t frame_index
) const {
    return RuntimeDiagnosticsSnapshot{
        frame_index,
        std::vector<RuntimeDiagnosticRecord>(records_.begin(), records_.end()),
        dropped_count_,
    };
}

std::uint64_t RuntimeDiagnosticStore::dropped_count() const noexcept { return dropped_count_; }

void RuntimeDiagnosticStore::clear() noexcept {
    records_.clear();
    next_sequence_ = 1U;
    dropped_count_ = 0U;
}

} // namespace strata::runtime
