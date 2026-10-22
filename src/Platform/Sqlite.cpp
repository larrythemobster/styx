#include "Sqlite.hpp"

#include <sqlite3.h>

namespace Styx::Platform {

// ---------------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------------

Statement::Statement(sqlite3_stmt *stmt, sqlite3 *db)
    : m_stmt(stmt)
    , m_db(db)
{
}

Statement::~Statement()
{
    if (m_stmt) {
        sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
    }
}

Statement::Statement(Statement &&other) noexcept
    : m_stmt(other.m_stmt)
    , m_db(other.m_db)
{
    other.m_stmt = nullptr;
    other.m_db = nullptr;
}

Statement &Statement::operator=(Statement &&other) noexcept
{
    if (this != &other) {
        if (m_stmt) {
            sqlite3_finalize(m_stmt);
        }
        m_stmt = other.m_stmt;
        m_db = other.m_db;
        other.m_stmt = nullptr;
        other.m_db = nullptr;
    }
    return *this;
}

void Statement::checkValid() const
{
    if (!m_stmt) {
        throw SqliteError(SQLITE_MISUSE, "Statement is not prepared or already finalized");
    }
}

int Statement::parameterIndex(std::string_view name) const
{
    checkValid();
    std::string n(name);
    int idx = sqlite3_bind_parameter_index(m_stmt, n.c_str());
    if (idx == 0) {
        throw SqliteError(SQLITE_NOTFOUND, "Unknown parameter name: " + n);
    }
    return idx;
}

void Statement::reset()
{
    checkValid();
    sqlite3_reset(m_stmt);
}

void Statement::clearBindings()
{
    checkValid();
    sqlite3_clear_bindings(m_stmt);
}

bool Statement::step()
{
    checkValid();
    int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    std::string msg = m_db ? sqlite3_errmsg(m_db) : "sqlite3_step failed";
    throw SqliteError(rc, msg);
}

void Statement::execute()
{
    checkValid();
    while (step()) {
    }
    reset();
}

void Statement::bind(int index, int val)
{
    checkValid();
    int rc = sqlite3_bind_int(m_stmt, index, val);
    if (rc != SQLITE_OK) {
        throw SqliteError(rc, m_db ? sqlite3_errmsg(m_db) : "bind int failed");
    }
}

void Statement::bind(int index, int64_t val)
{
    checkValid();
    int rc = sqlite3_bind_int64(m_stmt, index, val);
    if (rc != SQLITE_OK) {
        throw SqliteError(rc, m_db ? sqlite3_errmsg(m_db) : "bind int64 failed");
    }
}

void Statement::bind(int index, double val)
{
    checkValid();
    int rc = sqlite3_bind_double(m_stmt, index, val);
    if (rc != SQLITE_OK) {
        throw SqliteError(rc, m_db ? sqlite3_errmsg(m_db) : "bind double failed");
    }
}

void Statement::bind(int index, std::string_view val)
{
    checkValid();
    int rc = sqlite3_bind_text(m_stmt, index, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw SqliteError(rc, m_db ? sqlite3_errmsg(m_db) : "bind text failed");
    }
}

void Statement::bind(int index, const void *blob, size_t size)
{
    checkValid();
    int rc = sqlite3_bind_blob(m_stmt, index, blob, static_cast<int>(size), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw SqliteError(rc, m_db ? sqlite3_errmsg(m_db) : "bind blob failed");
    }
}

void Statement::bindNull(int index)
{
    checkValid();
    int rc = sqlite3_bind_null(m_stmt, index);
    if (rc != SQLITE_OK) {
        throw SqliteError(rc, m_db ? sqlite3_errmsg(m_db) : "bind null failed");
    }
}

void Statement::bind(std::string_view name, int val)
{
    bind(parameterIndex(name), val);
}

void Statement::bind(std::string_view name, int64_t val)
{
    bind(parameterIndex(name), val);
}

void Statement::bind(std::string_view name, double val)
{
    bind(parameterIndex(name), val);
}

void Statement::bind(std::string_view name, std::string_view val)
{
    bind(parameterIndex(name), val);
}

void Statement::bind(std::string_view name, const void *blob, size_t size)
{
    bind(parameterIndex(name), blob, size);
}

void Statement::bindNull(std::string_view name)
{
    bindNull(parameterIndex(name));
}

int Statement::getInt(int col) const
{
    checkValid();
    return sqlite3_column_int(m_stmt, col);
}

int64_t Statement::getInt64(int col) const
{
    checkValid();
    return sqlite3_column_int64(m_stmt, col);
}

double Statement::getDouble(int col) const
{
    checkValid();
    return sqlite3_column_double(m_stmt, col);
}

std::string Statement::getText(int col) const
{
    checkValid();
    const unsigned char *text = sqlite3_column_text(m_stmt, col);
    if (!text) {
        return {};
    }
    int bytes = sqlite3_column_bytes(m_stmt, col);
    return std::string(reinterpret_cast<const char *>(text), static_cast<size_t>(bytes));
}

std::string_view Statement::getTextView(int col) const
{
    checkValid();
    const unsigned char *text = sqlite3_column_text(m_stmt, col);
    if (!text) {
        return {};
    }
    int bytes = sqlite3_column_bytes(m_stmt, col);
    return std::string_view(reinterpret_cast<const char *>(text), static_cast<size_t>(bytes));
}

std::vector<uint8_t> Statement::getBlob(int col) const
{
    checkValid();
    const void *blob = sqlite3_column_blob(m_stmt, col);
    int bytes = sqlite3_column_bytes(m_stmt, col);
    if (!blob || bytes <= 0) {
        return {};
    }
    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(blob);
    return std::vector<uint8_t>(ptr, ptr + bytes);
}

bool Statement::isNull(int col) const
{
    checkValid();
    return sqlite3_column_type(m_stmt, col) == SQLITE_NULL;
}

int Statement::columnCount() const
{
    checkValid();
    return sqlite3_column_count(m_stmt);
}

std::string Statement::columnName(int col) const
{
    checkValid();
    const char *name = sqlite3_column_name(m_stmt, col);
    return name ? std::string(name) : std::string();
}

int Statement::columnType(int col) const
{
    checkValid();
    return sqlite3_column_type(m_stmt, col);
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

Database::Database() = default;

Database::Database(const std::filesystem::path &path, int flags)
{
    open(path, flags);
}

Database::~Database()
{
    close();
}

Database::Database(Database &&other) noexcept
    : m_db(other.m_db)
{
    other.m_db = nullptr;
}

Database &Database::operator=(Database &&other) noexcept
{
    if (this != &other) {
        close();
        m_db = other.m_db;
        other.m_db = nullptr;
    }
    return *this;
}

bool Database::open(const std::filesystem::path &path, int flags)
{
    close();
    auto u8 = path.u8string();
    std::string u8Path(reinterpret_cast<const char *>(u8.data()), u8.size());
    int rc = sqlite3_open_v2(u8Path.c_str(), &m_db, flags, nullptr);
    if (rc != SQLITE_OK) {
        if (m_db) {
            sqlite3_close_v2(m_db);
            m_db = nullptr;
        }
        return false;
    }
    return true;
}

void Database::close()
{
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

Statement Database::prepare(std::string_view sql)
{
    if (!m_db) {
        throw SqliteError(SQLITE_MISUSE, "Database is not open");
    }
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw SqliteError(rc, sqlite3_errmsg(m_db));
    }
    return Statement(stmt, m_db);
}

void Database::execute(std::string_view sql)
{
    if (!m_db) {
        throw SqliteError(SQLITE_MISUSE, "Database is not open");
    }
    char *errmsg = nullptr;
    std::string s(sql);
    int rc = sqlite3_exec(m_db, s.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "sqlite3_exec failed";
        if (errmsg) {
            sqlite3_free(errmsg);
        }
        throw SqliteError(rc, msg);
    }
}

int64_t Database::lastInsertRowId() const
{
    return m_db ? sqlite3_last_insert_rowid(m_db) : 0;
}

int Database::changes() const
{
    return m_db ? sqlite3_changes(m_db) : 0;
}

int Database::lastErrorCode() const
{
    return m_db ? sqlite3_errcode(m_db) : SQLITE_OK;
}

std::string Database::lastErrorMessage() const
{
    return m_db ? sqlite3_errmsg(m_db) : std::string();
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::Transaction(Database &db)
    : m_db(&db)
{
    if (!m_db->isOpen()) {
        throw SqliteError(SQLITE_MISUSE, "Cannot start transaction on closed database");
    }
    m_db->execute("BEGIN IMMEDIATE");
}

Transaction::~Transaction()
{
    if (!isFinished() && m_db && m_db->isOpen()) {
        try {
            rollback();
        } catch (...) {
        }
    }
}

Transaction::Transaction(Transaction &&other) noexcept
    : m_db(other.m_db)
    , m_committed(other.m_committed)
    , m_rolledBack(other.m_rolledBack)
{
    other.m_db = nullptr;
    other.m_committed = true;
    other.m_rolledBack = true;
}

Transaction &Transaction::operator=(Transaction &&other) noexcept
{
    if (this != &other) {
        if (!isFinished() && m_db && m_db->isOpen()) {
            try {
                rollback();
            } catch (...) {
            }
        }
        m_db = other.m_db;
        m_committed = other.m_committed;
        m_rolledBack = other.m_rolledBack;
        other.m_db = nullptr;
        other.m_committed = true;
        other.m_rolledBack = true;
    }
    return *this;
}

void Transaction::commit()
{
    if (isFinished()) {
        throw SqliteError(SQLITE_MISUSE, "Transaction already completed");
    }
    m_db->execute("COMMIT");
    m_committed = true;
}

void Transaction::rollback()
{
    if (isFinished()) {
        throw SqliteError(SQLITE_MISUSE, "Transaction already completed");
    }
    m_db->execute("ROLLBACK");
    m_rolledBack = true;
}

} // namespace Styx::Platform
