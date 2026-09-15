#include "recovery.h"

#include <unordered_set>

namespace flintdb {

size_t RunRecovery(const std::vector<LogRecord>& records, DiskManager* disk_manager) {
    std::unordered_set<TxnId> committed;
    for (const auto& record : records) {
        if (record.type == LogRecordType::kCommit) {
            committed.insert(record.txn_id);
        }
    }

    size_t replayed = 0;
    for (const auto& record : records) {
        if (record.type != LogRecordType::kUpdate) continue;
        if (committed.find(record.txn_id) == committed.end()) continue;
        disk_manager->WritePage(record.page_id, record.page_image.data());
        replayed++;
    }
    return replayed;
}

}  // namespace flintdb
