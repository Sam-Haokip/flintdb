#include "transaction.h"

namespace flintdb {

namespace {

// thread_local, not a plain global: each thread gets its own independent
// copy, which is exactly what the thread-per-transaction model needs --
// see transaction.h's class comment. Deliberately in this .cpp's own
// anonymous namespace rather than exposed in the header, so the only way
// to touch it at all is through the two free functions below.
thread_local Transaction* g_current_txn = nullptr;

}  // namespace

Transaction::~Transaction() {
    if (g_current_txn == this) g_current_txn = nullptr;
}

Transaction* GetCurrentTransaction() { return g_current_txn; }

void SetCurrentTransactionForThisThread(Transaction* txn) { g_current_txn = txn; }

}  // namespace flintdb
