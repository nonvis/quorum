#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <iostream>

#include <sqlite3.h>

namespace sui::quorum {

class Database {
public:
    explicit Database(const std::string& path) {
        int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "ERROR: sqlite3_open: " << sqlite3_errmsg(db_) << "\n";
            db_ = nullptr;
            return;
        }
        exec_raw("PRAGMA journal_mode=WAL");
        exec_raw("PRAGMA synchronous=NORMAL");
        exec_raw("PRAGMA foreign_keys=ON");
        exec_raw("PRAGMA busy_timeout=5000");
    }

    ~Database() {
        if (db_) sqlite3_close(db_);
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] bool is_open() const { return db_ != nullptr; }

    void execute(const std::string& sql) {
        std::lock_guard<std::mutex> lock(mutex_);
        exec_raw(sql);
    }

    void execute(const std::string& sql, std::function<void(sqlite3_stmt*)> bind) {
        std::lock_guard<std::mutex> lock(mutex_);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "ERROR: prepare: " << sqlite3_errmsg(db_) << "\n";
            return;
        }
        bind(stmt);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    void query(const std::string& sql,
               std::function<void(sqlite3_stmt*)> bind,
               std::function<void(sqlite3_stmt*)> row_callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "ERROR: prepare: " << sqlite3_errmsg(db_) << "\n";
            return;
        }
        bind(stmt);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            row_callback(stmt);
        }
        sqlite3_finalize(stmt);
    }

    void query(const std::string& sql,
               std::function<void(sqlite3_stmt*)> row_callback) {
        query(sql, [](sqlite3_stmt*){}, row_callback);
    }

    class Transaction {
    public:
        explicit Transaction(Database& db) : db_(db) {
            db_.execute("BEGIN TRANSACTION");
        }
        ~Transaction() {
            if (!committed_) db_.execute("ROLLBACK");
        }
        void commit() {
            db_.execute("COMMIT");
            committed_ = true;
        }
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
    private:
        Database& db_;
        bool committed_ = false;
    };

    sqlite3* handle() { return db_; }

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;

    void exec_raw(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::cerr << "ERROR: exec: " << (err ? err : "unknown") << "\n";
            if (err) sqlite3_free(err);
        }
    }
};

} // namespace sui::quorum
