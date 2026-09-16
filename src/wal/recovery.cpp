#include "recovery.h"

#include <unordered_set>

namespace flintdb {

size_t RunRecovery(const std::vector<LogRecord>& records,
                    const std::unordered_map<ObjectId, DiskManager*>& disk_managers_by_object) {
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
        // .at() throws std::out_of_range if record.object_id isn't in the
        // map -- see recovery.h's doc comment for why that's the right
        // failure mode here, not a silent skip.
        DiskManager* disk_manager = disk_managers_by_object.at(record.object_id);
        disk_manager->WritePage(record.page_id, record.page_image.data());
        replayed++;
    }
    return replayed;
}

}  // namespace flintdb
