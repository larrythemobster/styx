#pragma once

#include "Platform/Sqlite.hpp"

namespace Styx {
namespace Storage {

class Transaction
{
public:
    explicit Transaction(Platform::Database &db);
    ~Transaction();

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    bool commit();

    bool ownsTransaction() const { return owns; }

private:
    Platform::Database &db;
    void *dbHandle = nullptr;
    bool owns = false;
    bool finished = false;
};

} // namespace Storage
} // namespace Styx
