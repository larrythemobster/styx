#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace Styx::Platform {

class SqliteError : public std::runtime_error {
public:
    SqliteError(int code, std::string message)
        : std::runtime_error("SQLite error (" + std::to_string(code) + "): " + message)
        , m_code(code)
        , m_message(std::move(message))
    {
    }

    int code() const { return m_code; }
    const std::string &message() const { return m_message; }

private:
    int m_code;
    std::string m_message;
};

class Database;

class Statement {
public:
    Statement() = default;
    explicit Statement(sqlite3_stmt *stmt, sqlite3 *db = nullptr);
    ~Statement();

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    Statement(Statement &&other) noexcept;
    Statement &operator=(Statement &&other) noexcept;

    // Reset statement and clear bindings
    void reset();
    void clearBindings();

    // Execution
    bool step(); // returns true on row, false on done; throws SqliteError on error
    void execute(); // steps until done, resets automatically

    // 1-based parameter binding
    void bind(int index, int val);
    void bind(int index, int64_t val);
    void bind(int index, double val);
    void bind(int index, std::string_view val);
    void bind(int index, const void *blob, size_t size);
    void bindNull(int index);

    // Named parameter binding (e.g. ":id", "@name")
    void bind(std::string_view name, int val);
    void bind(std::string_view name, int64_t val);
    void bind(std::string_view name, double val);
    void bind(std::string_view name, std::string_view val);
    void bind(std::string_view name, const void *blob, size_t size);
    void bindNull(std::string_view name);

    // 0-based column access
    int getInt(int col) const;
    int64_t getInt64(int col) const;
    double getDouble(int col) const;
    std::string getText(int col) const;
    std::string_view getTextView(int col) const;
    std::vector<uint8_t> getBlob(int col) const;
    bool isNull(int col) const;

    int columnCount() const;
    std::string columnName(int col) const;
    int columnType(int col) const;

    bool isValid() const { return m_stmt != nullptr; }

private:
    int parameterIndex(std::string_view name) const;
    void checkValid() const;

    sqlite3_stmt *m_stmt{nullptr};
    sqlite3 *m_db{nullptr};
};

class Database {
public:
    Database();
    explicit Database(const std::filesystem::path &path, int flags = 6 /* SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE */);
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    Database(Database &&other) noexcept;
    Database &operator=(Database &&other) noexcept;

    bool open(const std::filesystem::path &path, int flags = 6);
    void close();

    bool isOpen() const { return m_db != nullptr; }

    Statement prepare(std::string_view sql);
    void execute(std::string_view sql);

    int64_t lastInsertRowId() const;
    int changes() const;

    int lastErrorCode() const;
    std::string lastErrorMessage() const;

    sqlite3 *rawHandle() const { return m_db; }

private:
    sqlite3 *m_db{nullptr};
};

class Transaction {
public:
    explicit Transaction(Database &db);
    ~Transaction();

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    Transaction(Transaction &&other) noexcept;
    Transaction &operator=(Transaction &&other) noexcept;

    void commit();
    void rollback();

    bool isFinished() const { return m_committed || m_rolledBack; }

private:
    Database *m_db{nullptr};
    bool m_committed{false};
    bool m_rolledBack{false};
};

} // namespace Styx::Platform
