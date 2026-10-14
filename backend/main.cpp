#include <cstdlib>
#include "httplib.h"
#include "json.hpp"
#include "sqlite3.h"
#include <iostream>
#include <string>
#include <sstream>
#include <random>
#include <chrono>
#include <mutex>
#include <ctime>
#include <iomanip>
#include <functional>

using json = nlohmann::json;
using namespace httplib;

sqlite3* db = nullptr;
std::mutex db_mutex;

const double DAILY_WITHDRAWAL_LIMIT = 5000.0;
const std::string ADMIN_PIN = "9999";

std::string nowTimestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::string today() {
    return nowTimestamp().substr(0, 10);
}

std::string hashPin(const std::string& pin) {
    // Demo only. For a production banking system use Argon2/bcrypt/scrypt.
    std::size_t h = std::hash<std::string>{}(pin + "bank_demo_salt_2026");
    std::ostringstream out;
    out << std::hex << h;
    return out.str();
}

std::string randomHex(std::size_t n) {
    static std::mt19937_64 rng(std::random_device{}());
    static const char* chars = "0123456789abcdef";
    std::uniform_int_distribution<int> d(0, 15);
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) s += chars[d(rng)];
    return s;
}
std::string generateAccountNumber() {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> d(100000, 999999);
    return "ACC" + std::to_string(d(rng));
}
bool execSQL(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "SQLite error: " << (err ? err : "unknown") << "\n";
        sqlite3_free(err);
        return false;
    }
    return true;
}

void initDB() {
    if (sqlite3_open("bank.db", &db) != SQLITE_OK) {
        std::cerr << "Cannot open database.\n";
        std::exit(1);
    }

    execSQL("PRAGMA foreign_keys = ON;");
    execSQL("PRAGMA journal_mode = WAL;");

    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS accounts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            account_number TEXT UNIQUE NOT NULL,
            name TEXT NOT NULL,
            pin_hash TEXT NOT NULL,
            balance REAL NOT NULL DEFAULT 0,
            is_frozen INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS sessions (
            token TEXT PRIMARY KEY,
            account_number TEXT NOT NULL,
            is_admin INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS transactions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            account_number TEXT NOT NULL,
            type TEXT NOT NULL,
            amount REAL NOT NULL,
            balance_after REAL NOT NULL,
            related_account TEXT DEFAULT '',
            timestamp TEXT NOT NULL,
            description TEXT DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS daily_withdrawals (
            account_number TEXT NOT NULL,
            date TEXT NOT NULL,
            total REAL NOT NULL DEFAULT 0,
            PRIMARY KEY(account_number, date)
        );
    )SQL";

    if (!execSQL(schema)) std::exit(1);
}

struct Account {
    bool found = false;
    std::string number, name, pinHash, createdAt;
    double balance = 0;
    int frozen = 0;
};

Account getAccount(const std::string& number) {
    Account a;
    const char* sql =
        "SELECT account_number,name,pin_hash,balance,is_frozen,created_at "
        "FROM accounts WHERE account_number=?";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return a;
    sqlite3_bind_text(st, 1, number.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(st) == SQLITE_ROW) {
        a.found = true;
        a.number = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        a.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        a.pinHash = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        a.balance = sqlite3_column_double(st, 3);
        a.frozen = sqlite3_column_int(st, 4);
        a.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    }
    sqlite3_finalize(st);
    return a;
}

bool accountExists(const std::string& number) {
    return getAccount(number).found;
}

void logTx(const std::string& acc, const std::string& type, double amount,
           double balanceAfter, const std::string& related,
           const std::string& description) {
    const char* sql =
        "INSERT INTO transactions(account_number,type,amount,balance_after,"
        "related_account,timestamp,description) VALUES(?,?,?,?,?,?,?)";

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    sqlite3_bind_text(st, 1, acc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 3, amount);
    sqlite3_bind_double(st, 4, balanceAfter);
    sqlite3_bind_text(st, 5, related.c_str(), -1, SQLITE_TRANSIENT);
    std::string ts = nowTimestamp();
    sqlite3_bind_text(st, 6, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

double dailyWithdrawn(const std::string& acc) {
    const char* sql =
        "SELECT total FROM daily_withdrawals WHERE account_number=? AND date=?";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    std::string d = today();
    sqlite3_bind_text(st, 1, acc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, d.c_str(), -1, SQLITE_TRANSIENT);

    double total = 0;
    if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_double(st, 0);
    sqlite3_finalize(st);
    return total;
}

bool addDailyWithdraw(const std::string& acc, double amount) {
    const char* sql =
        "INSERT INTO daily_withdrawals(account_number,date,total) VALUES(?,?,?) "
        "ON CONFLICT(account_number,date) DO UPDATE SET total=total+excluded.total";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    std::string d = today();
    sqlite3_bind_text(st, 1, acc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, d.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 3, amount);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

std::string tokenFromRequest(const Request& req) {
    if (!req.has_header("Authorization")) return "";
    std::string value = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (value.rfind(prefix, 0) == 0) return value.substr(prefix.size());
    return value;
}

struct Session {
    bool valid = false;
    bool admin = false;
    std::string account;
};

Session validateToken(const std::string& token) {
    Session s;
    if (token.empty()) return s;

    const char* sql =
        "SELECT account_number,is_admin FROM sessions WHERE token=?";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    sqlite3_bind_text(st, 1, token.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(st) == SQLITE_ROW) {
        s.valid = true;
        s.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        s.admin = sqlite3_column_int(st, 1) == 1;
    }
    sqlite3_finalize(st);
    return s;
}

void jsonResponse(Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

bool positiveAmount(double amount) {
    return amount > 0 && amount <= 1000000000.0;
}

int main() {
    initDB();
    Server svr;

    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"}
    });

    svr.Options(R"(/.*)", [](const Request&, Response& res) {
        res.status = 204;
    });

    // Frontend is served by the same C++ server.
    svr.set_mount_point("/", "./frontend");

    // ---------- REGISTER ----------
    svr.Post("/api/register", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            json b = json::parse(req.body);
            std::string name = b.value("name", "");
            std::string pin = b.value("pin", "");
            double initial = b.value("initial_deposit", 0.0);

            if (name.empty() || pin.size() < 4 || initial < 0) {
                return jsonResponse(res, 400,
                    {{"error", "Enter a name, PIN of at least 4 digits, and valid deposit."}});
            }

            std::string acc;
            do { acc = generateAccountNumber(); } while (accountExists(acc));

            const char* sql =
                "INSERT INTO accounts(account_number,name,pin_hash,balance,created_at)"
                "VALUES(?,?,?,?,?)";
            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
            std::string hashed = hashPin(pin);
            std::string ts = nowTimestamp();

            sqlite3_bind_text(st, 1, acc.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, hashed.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(st, 4, initial);
            sqlite3_bind_text(st, 5, ts.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(st) != SQLITE_DONE) {
                sqlite3_finalize(st);
                return jsonResponse(res, 500, {{"error", "Could not create account."}});
            }
            sqlite3_finalize(st);

            if (initial > 0)
                logTx(acc, "DEPOSIT", initial, initial, "", "Initial deposit");

            return jsonResponse(res, 201, {
                {"success", true},
                {"account_number", acc},
                {"message", "Account created successfully."}
            });
        } catch (...) {
            jsonResponse(res, 400, {{"error", "Invalid request body."}});
        }
    });

    // ---------- LOGIN ----------
    svr.Post("/api/login", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            json b = json::parse(req.body);
            std::string acc = b.value("account_number", "");
            std::string pin = b.value("pin", "");

            bool admin = false;
            if (acc == "ADMIN") {
                if (pin != ADMIN_PIN)
                    return jsonResponse(res, 401, {{"error", "Invalid admin PIN."}});
                admin = true;
            } else {
                Account a = getAccount(acc);
                if (!a.found || a.pinHash != hashPin(pin))
                    return jsonResponse(res, 401, {{"error", "Invalid account number or PIN."}});
                if (a.frozen)
                    return jsonResponse(res, 403, {{"error", "Account is frozen."}});
            }

            std::string token = randomHex(64);
            const char* sql =
                "INSERT INTO sessions(token,account_number,is_admin,created_at)"
                "VALUES(?,?,?,?)";
            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
            std::string ts = nowTimestamp();

            sqlite3_bind_text(st, 1, token.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, acc.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 3, admin ? 1 : 0);
            sqlite3_bind_text(st, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);

            return jsonResponse(res, 200, {
                {"success", true}, {"token", token}, {"is_admin", admin}
            });
        } catch (...) {
            jsonResponse(res, 400, {{"error", "Invalid request."}});
        }
    });

    // ---------- LOGOUT ----------
    svr.Post("/api/logout", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        std::string token = tokenFromRequest(req);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE token=?", -1, &st, nullptr);
        sqlite3_bind_text(st, 1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
        jsonResponse(res, 200, {{"success", true}});
    });

    // ---------- PROFILE ----------
    svr.Get("/api/profile", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        Session s = validateToken(tokenFromRequest(req));
        if (!s.valid || s.admin)
            return jsonResponse(res, 401, {{"error", "Unauthorized."}});

        Account a = getAccount(s.account);
        if (!a.found)
            return jsonResponse(res, 404, {{"error", "Account not found."}});

        jsonResponse(res, 200, {
            {"account_number", a.number},
            {"name", a.name},
            {"balance", a.balance},
            {"is_frozen", a.frozen},
            {"created_at", a.createdAt}
        });
    });

    // ---------- DEPOSIT ----------
    svr.Post("/api/deposit", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        Session s = validateToken(tokenFromRequest(req));
        if (!s.valid || s.admin)
            return jsonResponse(res, 401, {{"error", "Unauthorized."}});

        try {
            double amount = json::parse(req.body).value("amount", 0.0);
            if (!positiveAmount(amount))
                return jsonResponse(res, 400, {{"error", "Invalid amount."}});

            Account a = getAccount(s.account);
            if (!a.found || a.frozen)
                return jsonResponse(res, 403, {{"error", "Account unavailable."}});

            double newBalance = a.balance + amount;
            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(db,
                "UPDATE accounts SET balance=? WHERE account_number=?",
                -1, &st, nullptr);
            sqlite3_bind_double(st, 1, newBalance);
            sqlite3_bind_text(st, 2, s.account.c_str(), -1, SQLITE_TRANSIENT);
            bool ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);

            if (!ok) return jsonResponse(res, 500, {{"error", "Deposit failed."}});
            logTx(s.account, "DEPOSIT", amount, newBalance, "", "Cash deposit");

            jsonResponse(res, 200, {
                {"success", true}, {"balance", newBalance}
            });
        } catch (...) {
            jsonResponse(res, 400, {{"error", "Invalid request."}});
        }
    });

    // ---------- WITHDRAW ----------
    svr.Post("/api/withdraw", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        Session s = validateToken(tokenFromRequest(req));
        if (!s.valid || s.admin)
            return jsonResponse(res, 401, {{"error", "Unauthorized."}});

        try {
            double amount = json::parse(req.body).value("amount", 0.0);
            if (!positiveAmount(amount))
                return jsonResponse(res, 400, {{"error", "Invalid amount."}});

            Account a = getAccount(s.account);
            if (!a.found || a.frozen)
                return jsonResponse(res, 403, {{"error", "Account unavailable."}});
            if (amount > a.balance)
                return jsonResponse(res, 400, {{"error", "Insufficient balance."}});

            double used = dailyWithdrawn(s.account);
            if (used + amount > DAILY_WITHDRAWAL_LIMIT)
                return jsonResponse(res, 400, {
                    {"error", "Daily withdrawal limit is ₹5000."},
                    {"remaining", DAILY_WITHDRAWAL_LIMIT - used}
                });

            double newBalance = a.balance - amount;

            if (!execSQL("BEGIN IMMEDIATE;"))
                return jsonResponse(res, 500, {{"error", "Transaction could not start."}});

            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(db,
                "UPDATE accounts SET balance=? WHERE account_number=?",
                -1, &st, nullptr);
            sqlite3_bind_double(st, 1, newBalance);
            sqlite3_bind_text(st, 2, s.account.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(st) != SQLITE_DONE || !addDailyWithdraw(s.account, amount)) {
                sqlite3_finalize(st);
                execSQL("ROLLBACK;");
                return jsonResponse(res, 500, {{"error", "Withdrawal failed."}});
            }
            sqlite3_finalize(st);
            logTx(s.account, "WITHDRAW", amount, newBalance, "", "Cash withdrawal");
            if (!execSQL("COMMIT;")) {
                execSQL("ROLLBACK;");
                return jsonResponse(res, 500, {{"error", "Withdrawal could not be committed."}});
            }

            jsonResponse(res, 200, {
                {"success", true}, {"balance", newBalance}
            });
        } catch (...) {
            execSQL("ROLLBACK;");
            jsonResponse(res, 400, {{"error", "Invalid request."}});
        }
    });

    // ---------- TRANSFER ----------
    svr.Post("/api/transfer", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        Session s = validateToken(tokenFromRequest(req));
        if (!s.valid || s.admin)
            return jsonResponse(res, 401, {{"error", "Unauthorized."}});

        try {
            json b = json::parse(req.body);
            std::string to = b.value("to_account", "");
            double amount = b.value("amount", 0.0);

            if (to.empty() || !positiveAmount(amount))
                return jsonResponse(res, 400, {{"error", "Enter a valid receiver and amount."}});
            if (to == s.account)
                return jsonResponse(res, 400, {{"error", "Cannot transfer to the same account."}});

            Account from = getAccount(s.account);
            Account toAcc = getAccount(to);

            if (!from.found || !toAcc.found)
                return jsonResponse(res, 404, {{"error", "Sender or receiver not found."}});
            if (from.frozen || toAcc.frozen)
                return jsonResponse(res, 403, {{"error", "Sender or receiver account is frozen."}});
            if (amount > from.balance)
                return jsonResponse(res, 400, {{"error", "Insufficient balance."}});

            // Both balance updates and both logs are committed atomically.
            if (!execSQL("BEGIN IMMEDIATE;"))
                return jsonResponse(res, 500, {{"error", "Could not start transfer."}});

            bool ok = true;
            sqlite3_stmt* st = nullptr;

            sqlite3_prepare_v2(db,
                "UPDATE accounts SET balance=balance-? WHERE account_number=?",
                -1, &st, nullptr);
            sqlite3_bind_double(st, 1, amount);
            sqlite3_bind_text(st, 2, s.account.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);

            if (ok) {
                sqlite3_prepare_v2(db,
                    "UPDATE accounts SET balance=balance+? WHERE account_number=?",
                    -1, &st, nullptr);
                sqlite3_bind_double(st, 1, amount);
                sqlite3_bind_text(st, 2, to.c_str(), -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(st) == SQLITE_DONE;
                sqlite3_finalize(st);
            }

            double senderAfter = from.balance - amount;
            double receiverAfter = toAcc.balance + amount;

            if (ok) {
                logTx(s.account, "TRANSFER_OUT", amount, senderAfter, to,
                      "Transfer to " + to);
                logTx(to, "TRANSFER_IN", amount, receiverAfter, s.account,
                      "Transfer from " + s.account);
                ok = execSQL("COMMIT;");
            }

            if (!ok) {
                execSQL("ROLLBACK;");
                return jsonResponse(res, 500, {{"error", "Transfer failed and was rolled back."}});
            }

            jsonResponse(res, 200, {
                {"success", true},
                {"balance", senderAfter},
                {"message", "Transfer successful."}
            });
        } catch (...) {
            execSQL("ROLLBACK;");
            jsonResponse(res, 400, {{"error", "Invalid request."}});
        }
    });

    // ---------- TRANSACTION HISTORY ----------
    svr.Get("/api/transactions", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        Session s = validateToken(tokenFromRequest(req));
        if (!s.valid || s.admin)
            return jsonResponse(res, 401, {{"error", "Unauthorized."}});

        int page = 1, limit = 10;
        if (req.has_param("page")) {
            try { page = std::max(1, std::stoi(req.get_param_value("page"))); } catch (...) {}
        }
        if (req.has_param("limit")) {
            try { limit = std::min(50, std::max(1, std::stoi(req.get_param_value("limit")))); } catch (...) {}
        }
        int offset = (page - 1) * limit;

        int total = 0;
        sqlite3_stmt* count = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM transactions WHERE account_number=?",
            -1, &count, nullptr);
        sqlite3_bind_text(count, 1, s.account.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(count) == SQLITE_ROW) total = sqlite3_column_int(count, 0);
        sqlite3_finalize(count);

        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT id,type,amount,balance_after,related_account,timestamp,description "
            "FROM transactions WHERE account_number=? ORDER BY id DESC LIMIT ? OFFSET ?",
            -1, &st, nullptr);
        sqlite3_bind_text(st, 1, s.account.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, limit);
        sqlite3_bind_int(st, 3, offset);

        json items = json::array();
        while (sqlite3_step(st) == SQLITE_ROW) {
            items.push_back({
                {"id", sqlite3_column_int(st, 0)},
                {"type", reinterpret_cast<const char*>(sqlite3_column_text(st, 1))},
                {"amount", sqlite3_column_double(st, 2)},
                {"balance_after", sqlite3_column_double(st, 3)},
                {"related_account", reinterpret_cast<const char*>(sqlite3_column_text(st, 4))},
                {"timestamp", reinterpret_cast<const char*>(sqlite3_column_text(st, 5))},
                {"description", reinterpret_cast<const char*>(sqlite3_column_text(st, 6))}
            });
        }
        sqlite3_finalize(st);

        jsonResponse(res, 200, {
            {"page", page}, {"limit", limit}, {"total", total}, {"transactions", items}
        });
    });

    // ---------- ADMIN: ACCOUNTS ----------
    svr.Get("/api/admin/accounts", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        Session s = validateToken(tokenFromRequest(req));
        if (!s.valid || !s.admin)
            return jsonResponse(res, 401, {{"error", "Admin access required."}});

        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT account_number,name,balance,is_frozen,created_at "
            "FROM accounts ORDER BY id DESC", -1, &st, nullptr);

        json accounts = json::array();
        while (sqlite3_step(st) == SQLITE_ROW) {
            accounts.push_back({
                {"account_number", reinterpret_cast<const char*>(sqlite3_column_text(st, 0))},
                {"name", reinterpret_cast<const char*>(sqlite3_column_text(st, 1))},
                {"balance", sqlite3_column_double(st, 2)},
                {"is_frozen", sqlite3_column_int(st, 3)},
                {"created_at", reinterpret_cast<const char*>(sqlite3_column_text(st, 4))}
            });
        }
        sqlite3_finalize(st);
        jsonResponse(res, 200, {{"accounts", accounts}});
    });

    // ---------- ADMIN: FREEZE/UNFREEZE ----------
    svr.Put(R"(/api/admin/accounts/(.+)/freeze)", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        Session s = validateToken(tokenFromRequest(req));
        if (!s.valid || !s.admin)
            return jsonResponse(res, 401, {{"error", "Admin access required."}});

        std::string acc = req.matches[1];
        if (!accountExists(acc))
            return jsonResponse(res, 404, {{"error", "Account not found."}});

        try {
            bool freeze = json::parse(req.body).value("freeze", true);
            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(db,
                "UPDATE accounts SET is_frozen=? WHERE account_number=?",
                -1, &st, nullptr);
            sqlite3_bind_int(st, 1, freeze ? 1 : 0);
            sqlite3_bind_text(st, 2, acc.c_str(), -1, SQLITE_TRANSIENT);
            bool ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);

            if (!ok) return jsonResponse(res, 500, {{"error", "Update failed."}});
            jsonResponse(res, 200, {
                {"success", true},
                {"message", freeze ? "Account frozen." : "Account unfrozen."}
            });
        } catch (...) {
            jsonResponse(res, 400, {{"error", "Invalid request."}});
        }
    });
    // ---------- ADMIN: DELETE ACCOUNT ----------
svr.Delete(R"(/api/admin/accounts/(.+))",
    [](const Request& req, Response& res) {

    std::lock_guard<std::mutex> lock(db_mutex);

    Session s = validateToken(tokenFromRequest(req));

    if (!s.valid || !s.admin)
        return jsonResponse(res, 401,
            {{"error", "Admin access required."}});

    std::string acc = req.matches[1];

    if (!accountExists(acc))
        return jsonResponse(res, 404,
            {{"error", "Account not found."}});

    // Prevent deleting an account that still has money
    Account a = getAccount(acc);

    if (a.balance > 0)
        return jsonResponse(res, 400,
            {{"error", "Cannot delete account with remaining balance. Withdraw or transfer the balance first."}});

    if (!execSQL("BEGIN IMMEDIATE;"))
        return jsonResponse(res, 500,
            {{"error", "Could not start delete transaction."}});

    bool ok = true;

    // Delete transactions
    sqlite3_stmt* st = nullptr;

    if (sqlite3_prepare_v2(
        db,
        "DELETE FROM transactions WHERE account_number=?",
        -1,
        &st,
        nullptr) != SQLITE_OK) {

        execSQL("ROLLBACK;");

        return jsonResponse(res, 500,
            {{"error", "Could not delete transaction history."}});
    }

    sqlite3_bind_text(
        st,
        1,
        acc.c_str(),
        -1,
        SQLITE_TRANSIENT
    );

    ok = sqlite3_step(st) == SQLITE_DONE;

    sqlite3_finalize(st);

    if (!ok) {
        execSQL("ROLLBACK;");

        return jsonResponse(res, 500,
            {{"error", "Could not delete transaction history."}});
    }

    // Delete daily withdrawal records
    if (ok) {

        if (sqlite3_prepare_v2(
            db,
            "DELETE FROM daily_withdrawals WHERE account_number=?",
            -1,
            &st,
            nullptr) != SQLITE_OK) {

            execSQL("ROLLBACK;");

            return jsonResponse(res, 500,
                {{"error", "Could not delete withdrawal records."}});
        }

        sqlite3_bind_text(
            st,
            1,
            acc.c_str(),
            -1,
            SQLITE_TRANSIENT
        );

        ok = sqlite3_step(st) == SQLITE_DONE;

        sqlite3_finalize(st);
    }

    if (!ok) {
        execSQL("ROLLBACK;");

        return jsonResponse(res, 500,
            {{"error", "Could not delete withdrawal records."}});
    }

    // Delete sessions
    if (sqlite3_prepare_v2(
        db,
        "DELETE FROM sessions WHERE account_number=?",
        -1,
        &st,
        nullptr) != SQLITE_OK) {

        execSQL("ROLLBACK;");

        return jsonResponse(res, 500,
            {{"error", "Could not delete sessions."}});
    }

    sqlite3_bind_text(
        st,
        1,
        acc.c_str(),
        -1,
        SQLITE_TRANSIENT
    );

    ok = sqlite3_step(st) == SQLITE_DONE;

    sqlite3_finalize(st);

    if (!ok) {
        execSQL("ROLLBACK;");

        return jsonResponse(res, 500,
            {{"error", "Could not delete sessions."}});
    }

    // Finally delete the account
    if (sqlite3_prepare_v2(
        db,
        "DELETE FROM accounts WHERE account_number=?",
        -1,
        &st,
        nullptr) != SQLITE_OK) {

        execSQL("ROLLBACK;");

        return jsonResponse(res, 500,
            {{"error", "Could not delete account."}});
    }

    sqlite3_bind_text(
        st,
        1,
        acc.c_str(),
        -1,
        SQLITE_TRANSIENT
    );

    ok = sqlite3_step(st) == SQLITE_DONE;

    sqlite3_finalize(st);

    if (!ok) {
        execSQL("ROLLBACK;");

        return jsonResponse(res, 500,
            {{"error", "Could not delete account."}});
    }

    if (!execSQL("COMMIT;")) {
        execSQL("ROLLBACK;");

        return jsonResponse(res, 500,
            {{"error", "Could not commit account deletion."}});
    }

    jsonResponse(res, 200, {
        {"success", true},
        {"message", "Account deleted successfully."}
    });
});

    // ---------- ADMIN: ALL TRANSACTIONS ----------
    svr.Get("/api/admin/transactions", [](const Request& req, Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        Session s = validateToken(tokenFromRequest(req));
        if (!s.valid || !s.admin)
            return jsonResponse(res, 401, {{"error", "Admin access required."}});

        int limit = 100;
        if (req.has_param("limit")) {
            try { limit = std::min(500, std::max(1, std::stoi(req.get_param_value("limit")))); } catch (...) {}
        }

        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT id,account_number,type,amount,balance_after,related_account,"
            "timestamp,description FROM transactions ORDER BY id DESC LIMIT ?",
            -1, &st, nullptr);
        sqlite3_bind_int(st, 1, limit);

        json items = json::array();
        while (sqlite3_step(st) == SQLITE_ROW) {
            items.push_back({
                {"id", sqlite3_column_int(st, 0)},
                {"account_number", reinterpret_cast<const char*>(sqlite3_column_text(st, 1))},
                {"type", reinterpret_cast<const char*>(sqlite3_column_text(st, 2))},
                {"amount", sqlite3_column_double(st, 3)},
                {"balance_after", sqlite3_column_double(st, 4)},
                {"related_account", reinterpret_cast<const char*>(sqlite3_column_text(st, 5))},
                {"timestamp", reinterpret_cast<const char*>(sqlite3_column_text(st, 6))},
                {"description", reinterpret_cast<const char*>(sqlite3_column_text(st, 7))}
            });
        }
        sqlite3_finalize(st);
        jsonResponse(res, 200, {{"transactions", items}});
    });

    std::cout << "====================================\n";
    std::cout << " Bank ATM Server\n";
    std::cout << " Open: http://localhost:8080\n";
    std::cout << " Admin account: ADMIN / 9999\n";
    std::cout << "====================================\n";

    const char* portEnv = std::getenv("PORT");
int port = portEnv ? std::stoi(portEnv) : 8080;
svr.listen("0.0.0.0", port);

    sqlite3_close(db);
    return 0;
}