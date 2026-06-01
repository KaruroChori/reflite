#pragma once

/**
 * @file reflite.hpp
 * @author karurochari
 * @brief Single header file library to wrap Sqlite3 using C++26 reflections
 * @date 2026-05-26
 * @license AGPL3.0
 * @copyright Copyright (c) 2026
 * 
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <meta>
#include <type_traits>
#include <expected>
#include <optional>

#include <sqlite3.h>

namespace reflite{

enum class db_type_t { 
    Auto,       //Pick default option contextually to the final C++ type, basically it prevent override.
    Integer, 
    Real, 
    Text, 
    Blob
};

struct column_t {
    db_type_t type = db_type_t::Auto;
    //bool pk = false;
    //bool unique = false;
    bool ignore = false;
    char name[64]{};
};

namespace details{

consteval column_t get_col_meta(std::meta::info mem) {
    auto annots = std::meta::annotations_of(mem);
    for (auto annot : annots) {
        if (is_same_type(remove_const(type_of(annot)), ^^column_t))
            return extract<column_t>(annot);
    }
    return {};
}

template <typename T, db_type_t ST = db_type_t::Auto> 
struct SqliteTypeMap {
    static int Bind(sqlite3_stmt* stmt, int idx, const T& val) {
        if constexpr (ST == db_type_t::Integer || (ST == db_type_t::Auto && std::is_integral_v<T>)) {
            return sqlite3_bind_int64(stmt, idx, static_cast<int64_t>(val));
        }
        else if constexpr (ST == db_type_t::Real || (ST == db_type_t::Auto && std::is_floating_point_v<T>)) {
            return sqlite3_bind_double(stmt, idx, static_cast<double>(val));
        }
        else if constexpr (ST == db_type_t::Text || (ST == db_type_t::Auto && (std::is_convertible_v<T, std::string_view> || std::is_same_v<std::remove_cvref_t<T>, const char*>))) {
            std::string_view sv = val; 
            return sqlite3_bind_text(stmt, idx, sv.data(), sv.size(), SQLITE_TRANSIENT);
        }
        else if constexpr (ST == db_type_t::Blob || (ST == db_type_t::Auto && std::is_same_v<T, std::vector<uint8_t>>)) {
            return sqlite3_bind_blob(stmt, idx, val.data(), val.size() * sizeof(decltype(val.data()[0])), SQLITE_TRANSIENT);
        }
        else {
            static_assert(false, "Unsupported SQLite Bind type.");
        }
    }

    static T Extract(sqlite3_stmt* stmt, int col) {
        if constexpr (ST == db_type_t::Integer || (ST == db_type_t::Auto && std::is_integral_v<T>)) {
            return static_cast<T>(sqlite3_column_int64(stmt, col));
        }
        else if constexpr (ST == db_type_t::Real || (ST == db_type_t::Auto && std::is_floating_point_v<T>)) {
            return static_cast<T>(sqlite3_column_double(stmt, col));
        }
        else if constexpr (ST == db_type_t::Text || (ST == db_type_t::Auto && std::is_constructible_v<T, std::string_view>)) {
            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            int len = sqlite3_column_bytes(stmt, col);
            if (!text) return T{};

            if constexpr (std::is_constructible_v<T, const char*, int>) {
                return T(text, len);
            } else if constexpr (std::is_constructible_v<T, std::string_view>) {
                return T(std::string_view(text, len));
            } else {
                return T(text); 
            }
        }
        else if constexpr (ST == db_type_t::Blob || (ST == db_type_t::Auto && std::is_same_v<T, std::vector<uint8_t>>)) {
            const void* blob = sqlite3_column_blob(stmt, col);
            int len = sqlite3_column_bytes(stmt, col);
            if (!blob) return T{};
            
            using ValT = std::remove_reference_t<decltype(std::declval<T>().data()[0])>;
            const ValT* ptr = static_cast<const ValT*>(blob);
            return T(ptr, ptr + (len / sizeof(ValT)));
        }
        else {
            static_assert(false, "Unsupported SQLite Extract type.");
        }
    }
};

template <typename T, db_type_t ST> 
struct SqliteTypeMap<std::optional<T>, ST> {
    static int Bind(sqlite3_stmt* stmt, int idx, const std::optional<T>& val) {
        if (!val.has_value()) return sqlite3_bind_null(stmt, idx);
        return SqliteTypeMap<T, ST>::Bind(stmt, idx, val.value());
    }
    static std::optional<T> Extract(sqlite3_stmt* stmt, int col) {
        if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
        return SqliteTypeMap<T, ST>::Extract(stmt, col);
    }
};

}

using sql = column_t;


class Database {
public:
    enum struct error_t { Ok, GenericError, PrepareError, BindError, StepError };
    bool log_errors = true;

    // Type-erased query executors
    template <typename In, typename Out = void> struct QueryInsert;
    template <typename In, typename Out = void> struct QueryUpdate;
    template <typename Out = void> struct QueryRemove;
    template <typename Out = void> struct QueryExtract;
    struct QueryRunner;

private:
    sqlite3* handle = nullptr;

    void log_if_error(int rc, std::string_view context) const {
        if (log_errors && rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
            //std::println(stderr, "SQLite Error [{}]: {} ({})", context, rc, handle ? sqlite3_errmsg(handle) : "N/A");
        } 
    }

public:
    ~Database() { auto _ = deinit(); }

    std::expected<std::monostate, error_t> init(const char* filepath, std::optional<int> flags = {}) {
        int rc = sqlite3_open_v2(filepath, &handle, flags.value_or(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE), nullptr);
        if (rc != SQLITE_OK) {
            log_if_error(rc, "init");
            return std::unexpected{error_t::GenericError};
        }
        return {};
    }

    std::expected<std::monostate, error_t> deinit() {
        if (handle) {
            int rc = sqlite3_close_v2(handle);
            if (rc != SQLITE_OK) {
                log_if_error(rc, "deinit");
                return std::unexpected{error_t::GenericError};
            }
            handle = nullptr;
        }
        return {};
    }

    std::expected<sqlite3_stmt*, error_t> prepare_or_cached(std::string_view query_str) {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(handle, query_str.data(), query_str.size(), &stmt, nullptr);
        if (rc != SQLITE_OK) {
            log_if_error(rc, "prepare_or_cached");
            return std::unexpected{error_t::PrepareError};
        }
        return stmt;
    }

    // --- Diagnostic helpers for complete queries if you don't want `RETURNING` ---

    int64_t rows_modified() const {
        return handle ? sqlite3_changes64(handle) : 0;
    }

    int64_t last_insert_id() const {
        return handle ? sqlite3_last_insert_rowid(handle) : 0;
    }

    // --- Statement builder for special or generic query types ---

    std::expected<QueryRunner, error_t> make_execute(std::string_view sql) {
        auto stmt = prepare_or_cached(sql);
        if (!stmt) return std::unexpected{stmt.error()};
        return QueryRunner{this, stmt.value()};
    }

    template <typename Out>
    std::expected<QueryExtract<Out>, error_t> make_query(std::string_view sql) {
        auto stmt = prepare_or_cached(sql);
        if (!stmt) return std::unexpected{stmt.error()};
        return QueryExtract<Out>{this, stmt.value()};
    }

    template <typename In, typename Out = void>
    std::expected<QueryInsert<In, Out>, error_t> make_insert(std::string_view table) {
        static constexpr auto members = define_static_array(std::meta::nonstatic_data_members_of(^^In, std::meta::access_context::unchecked()));
        std::string sql = "INSERT INTO ";
        sql.append(table).append(" (");
        std::string vals = ") VALUES (";
        bool first = true;
        
        template for (constexpr auto mem : members) {
            constexpr column_t meta = details::get_col_meta(mem);
            if constexpr (!meta.ignore) {
                if (!first) { sql += ", "; vals += ", "; }
                constexpr std::string_view mem_name = std::meta::identifier_of(mem);
                sql += (meta.name[0] != '\0' ? meta.name : mem_name);
                vals += "?";
                first = false;
            }
        }
        sql += vals + ")";

        if constexpr (!std::is_same_v<Out, void>) {
            sql += " RETURNING ";
            static constexpr auto out_members = define_static_array(std::meta::nonstatic_data_members_of(^^Out, std::meta::access_context::unchecked()));
            bool first_out = true;
            template for (constexpr auto mem : out_members) {
                constexpr column_t meta = details::get_col_meta(mem);
                if constexpr (!meta.ignore) {
                    if (!first_out) sql += ", ";
                    constexpr std::string_view mem_name = std::meta::identifier_of(mem);
                    sql += (meta.name[0] != '\0' ? meta.name : mem_name);
                    first_out = false;
                }
            }
        }
        sql += ";";

        auto stmt = prepare_or_cached(sql);
        if (!stmt) return std::unexpected{stmt.error()};
        return QueryInsert<In, Out>{this, stmt.value()};
    }

    template <typename In, typename Out = void>
    std::expected<QueryUpdate<In, Out>, error_t> make_update(std::string_view table, std::string_view where_clause = "") {
        static constexpr auto members = define_static_array(std::meta::nonstatic_data_members_of(^^In, std::meta::access_context::unchecked()));
        std::string sql = "UPDATE ";
        sql.append(table).append(" SET ");

        bool first_set = true;
        template for (constexpr auto mem : members) {
            constexpr column_t meta = details::get_col_meta(mem);
            if constexpr (!meta.ignore) {
                constexpr std::string_view mem_name = std::meta::identifier_of(mem);
                std::string_view col_name = meta.name[0] != '\0' ? std::string_view(meta.name) : mem_name;
            
                if (!first_set) sql += ", ";
                sql.append(col_name).append(" = ?");
                first_set = false;
            }
        }

        if (!where_clause.empty()) {
            sql += " ";
            sql += where_clause;
        }

        if constexpr (!std::is_same_v<Out, void>) {
            sql += " RETURNING ";
            static constexpr auto out_members = define_static_array(std::meta::nonstatic_data_members_of(^^Out, std::meta::access_context::unchecked()));
            bool first_out = true;
            template for (constexpr auto mem : out_members) {
                constexpr column_t meta = details::get_col_meta(mem);
                if constexpr (!meta.ignore) {
                    if (!first_out) sql += ", ";
                    constexpr std::string_view mem_name = std::meta::identifier_of(mem);
                    sql += (meta.name[0] != '\0' ? meta.name : mem_name);
                    first_out = false;
                }
            }
        }
        sql += ";";
        
        auto stmt = prepare_or_cached(sql);
        if (!stmt) return std::unexpected{stmt.error()};
        return QueryUpdate<In, Out>{this, stmt.value()};
    }

    template <typename Out = void>
    std::expected<QueryRemove<Out>, error_t> make_remove(std::string_view table, std::string_view where_clause = "") {
        std::string sql = "DELETE FROM ";
        sql.append(table);
        
        sql.append(" ");
        sql.append(where_clause);

        if constexpr (!std::is_same_v<Out, void>) {
            sql += " RETURNING ";
            static constexpr auto out_members = define_static_array(std::meta::nonstatic_data_members_of(^^Out, std::meta::access_context::unchecked()));
            bool first_out = true;
            template for (constexpr auto mem : out_members) {
                constexpr column_t meta = details::get_col_meta(mem);
                if constexpr (!meta.ignore) {
                    if (!first_out) sql += ", ";
                    constexpr std::string_view mem_name = std::meta::identifier_of(mem);
                    sql += (meta.name[0] != '\0' ? meta.name : mem_name);
                    first_out = false;
                }
            }
        }
        sql += ";";

        auto stmt = prepare_or_cached(sql);
        if (!stmt) return std::unexpected{stmt.error()};
        return QueryRemove<Out>{this, stmt.value()};
    }

    template <typename Out>
    std::expected<QueryExtract<Out>, error_t> make_select(std::string_view table, std::string_view where_clause = "") {
        static constexpr auto members = define_static_array(std::meta::nonstatic_data_members_of(^^Out, std::meta::access_context::unchecked()));
        std::string sql = "SELECT ";
        bool first = true;
        
        template for (constexpr auto mem : members) {
            constexpr column_t meta = details::get_col_meta(mem);
            if constexpr (!meta.ignore) {
                if (!first) sql += ", ";
                constexpr std::string_view mem_name = std::meta::identifier_of(mem);
                sql += (meta.name[0] != '\0' ? meta.name : mem_name);
                first = false;
            }
        }
        sql += " FROM ";
        sql += table;
        if (!where_clause.empty()) {
            sql += " ";
            sql += where_clause;
        }

        auto stmt = prepare_or_cached(sql);
        if (!stmt) return std::unexpected{stmt.error()};
        return QueryExtract<Out>{this, stmt.value()};
    }

    // --- Execution wrappers, to later finalize the reusable query ---

    struct QueryRunner {
        Database* db;
        sqlite3_stmt* stmt;

        template <typename... InArgs>
        std::expected<std::monostate, error_t> with(InArgs&&... args) const {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            int bind_idx = 1;
            bool bind_ok = true;
            (..., (bind_ok = bind_ok && (details::SqliteTypeMap<std::remove_cvref_t<InArgs>>::Bind(stmt, bind_idx++, args) == SQLITE_OK)));
            
            if (!bind_ok) {
                db->log_if_error(SQLITE_ERROR, "QueryRunner bind");
                return std::unexpected{error_t::BindError};
            }
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                db->log_if_error(SQLITE_ERROR, "QueryRunner step");
                return std::unexpected{error_t::StepError};
            }
            return std::monostate{};
        }

        QueryRunner(Database* db, sqlite3_stmt* stmt):db(db),stmt(stmt){}
        QueryRunner(QueryRunner&& other):db(other.db),stmt(other.stmt){other.stmt=nullptr;}
        ~QueryRunner(){ if(stmt)sqlite3_finalize(stmt); }
    };

    template <typename Out>
    struct QueryExtract {
        Database* db;
        sqlite3_stmt* stmt;

        template <typename... InArgs>
        std::expected<std::vector<Out>, error_t> with(InArgs&&... args) const {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            int bind_idx = 1;
            bool bind_ok = true;
            (..., (bind_ok = bind_ok && (details::SqliteTypeMap<std::remove_cvref_t<InArgs>>::Bind(stmt, bind_idx++, args) == SQLITE_OK)));
            if (!bind_ok) return std::unexpected{error_t::BindError};

            std::vector<Out> results;
            int rc;
            static constexpr auto members = define_static_array(std::meta::nonstatic_data_members_of(^^Out, std::meta::access_context::unchecked()));

            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                Out row;
                int col = 0;
                template for (constexpr auto mem : members) {
                    constexpr column_t meta = details::get_col_meta(mem);
                    if constexpr (!meta.ignore) {
                        using FieldType = [:remove_cvref(type_of(mem)):];
                        row.[:mem:] = details::SqliteTypeMap<FieldType, meta.type>::Extract(stmt, col++);
                    }
                }
                results.push_back(std::move(row));
            }
            if (rc != SQLITE_DONE){
                db->log_if_error(SQLITE_ERROR, "QueryExtract step");
                return std::unexpected{error_t::StepError};
            }
            return results;
        }

        QueryExtract(Database* db, sqlite3_stmt* stmt):db(db),stmt(stmt){}
        QueryExtract(QueryExtract&& other):db(other.db),stmt(other.stmt){other.stmt=nullptr;}
        ~QueryExtract(){ if(stmt)sqlite3_finalize(stmt); }
    };

    template <typename In, typename Out>
    struct QueryInsert {
        Database* db;
        sqlite3_stmt* stmt;

        using ReturnType = std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>;

        std::expected<ReturnType, error_t> with(const In& obj) const {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            int bind_idx = 1;
            static constexpr auto members = define_static_array(std::meta::nonstatic_data_members_of(^^In, std::meta::access_context::unchecked()));
            template for (constexpr auto mem : members) {
                constexpr column_t meta = details::get_col_meta(mem);
                if constexpr (!meta.ignore) {
                    using ValType = std::remove_cvref_t<decltype(obj.[:mem:])>;
                    if (details::SqliteTypeMap<ValType, meta.type>::Bind(stmt, bind_idx++, obj.[:mem:]) != SQLITE_OK) {
                        db->log_if_error(SQLITE_ERROR, "QueryInsert bind");
                        return std::unexpected{error_t::BindError};
                    }
                }
            }
            
            if constexpr (std::is_same_v<Out, void>) {
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    db->log_if_error(SQLITE_ERROR, "QueryInsert step");
                    return std::unexpected{error_t::StepError};
                }
                return std::monostate{};
            } else {
                std::vector<Out> results;
                int rc;
                static constexpr auto out_members = define_static_array(std::meta::nonstatic_data_members_of(^^Out, std::meta::access_context::unchecked()));

                while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                    Out row;
                    int col = 0;
                    template for (constexpr auto mem : out_members) {
                        constexpr column_t meta = details::get_col_meta(mem);
                        if constexpr (!meta.ignore) {
                            using FieldType = [:remove_cvref(type_of(mem)):];;
                            row.[:mem:] = details::SqliteTypeMap<FieldType, meta.type>::Extract(stmt, col++);
                        }
                    }
                    results.push_back(std::move(row));
                }
                if (rc != SQLITE_DONE){
                    db->log_if_error(SQLITE_ERROR, "QueryInsert step RETURNING");
                    return std::unexpected{error_t::StepError};
                }
                return results;
            }
        }

        QueryInsert(Database* db, sqlite3_stmt* stmt):db(db),stmt(stmt){}
        QueryInsert(QueryInsert&& other):db(other.db),stmt(other.stmt){other.stmt=nullptr;}
        ~QueryInsert(){ if(stmt)sqlite3_finalize(stmt); }
    };

    template <typename In, typename Out>
    struct QueryUpdate {
        Database* db;
        sqlite3_stmt* stmt;

        using ReturnType = std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>;

        template <typename... InArgs>
        std::expected<ReturnType, error_t> with(const In& obj, InArgs&&... args) const {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            int bind_idx = 1;
            bool bind_ok = true;

            static constexpr auto members = define_static_array(std::meta::nonstatic_data_members_of(^^In, std::meta::access_context::unchecked()));

            template for (constexpr auto mem : members) {
                constexpr column_t meta = details::get_col_meta(mem);
                if constexpr (!meta.ignore) {
                    using ValType = std::remove_cvref_t<decltype(obj.[:mem:])>;
                    if (details::SqliteTypeMap<ValType, meta.type>::Bind(stmt, bind_idx++, obj.[:mem:]) != SQLITE_OK) return std::unexpected{error_t::BindError};
                }
            }

            (..., (bind_ok = bind_ok && (details::SqliteTypeMap<std::remove_cvref_t<InArgs>>::Bind(stmt, bind_idx++, args) == SQLITE_OK)));
            if (!bind_ok) return std::unexpected{error_t::BindError};

            if constexpr (std::is_same_v<Out, void>) {
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    db->log_if_error(SQLITE_ERROR, "QueryUpdate step");
                    return std::unexpected{error_t::StepError};
                }
                return std::monostate{};
            } else {
                std::vector<Out> results;
                int rc;
                static constexpr auto out_members = define_static_array(std::meta::nonstatic_data_members_of(^^Out, std::meta::access_context::unchecked()));

                while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                    Out row;
                    int col = 0;
                    template for (constexpr auto mem : out_members) {
                        constexpr column_t meta = details::get_col_meta(mem);
                        if constexpr (!meta.ignore) {
                            using FieldType = [:remove_cvref(type_of(mem)):];
                            row.[:mem:] = details::SqliteTypeMap<FieldType, meta.type>::Extract(stmt, col++);
                        }
                    }
                    results.push_back(std::move(row));
                }
                if (rc != SQLITE_DONE){
                    db->log_if_error(SQLITE_ERROR, "QueryUpdate step RETURNING");
                    return std::unexpected{error_t::StepError};
                }
                return results;
            }
        }

        QueryUpdate(Database* db, sqlite3_stmt* stmt):db(db),stmt(stmt){}
        QueryUpdate(QueryUpdate&& other):db(other.db),stmt(other.stmt){other.stmt=nullptr;}
        ~QueryUpdate(){ if(stmt)sqlite3_finalize(stmt); }
    };

    template <typename Out>
    struct QueryRemove {
        Database* db;
        sqlite3_stmt* stmt;

        using ReturnType = std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>;

        template <typename... InArgs>
        std::expected<ReturnType, error_t> with(InArgs&&... args) const {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            int bind_idx = 1;
            bool bind_ok = true;

            (..., (bind_ok = bind_ok && (details::SqliteTypeMap<std::remove_cvref_t<InArgs>>::Bind(stmt, bind_idx++, args) == SQLITE_OK)));
            if (!bind_ok) return std::unexpected{error_t::BindError};

            if constexpr (std::is_same_v<Out, void>) {
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    db->log_if_error(SQLITE_ERROR, "QueryRemove step");
                    return std::unexpected{error_t::StepError};
                }
                return std::monostate{};
            } else {
                std::vector<Out> results;
                int rc;
                constexpr auto out_members = define_static_array(std::meta::nonstatic_data_members_of(^^Out, std::meta::access_context::unchecked()));

                while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                    Out row;
                    int col = 0;
                    template for (constexpr auto mem : out_members) {
                        constexpr column_t meta = details::get_col_meta(mem);
                        if constexpr (!meta.ignore) {
                            using FieldType = [:remove_cvref(type_of(mem)):];
                            row.[:mem:] = details::SqliteTypeMap<FieldType, meta.type>::Extract(stmt, col++);
                        }
                    }
                    results.push_back(std::move(row));
                }
                if (rc != SQLITE_DONE){
                    db->log_if_error(SQLITE_ERROR, "QueryRemove step RETURNING");
                    return std::unexpected{error_t::StepError};
                }
                return results;
            }
        }

        QueryRemove(Database* db, sqlite3_stmt* stmt):db(db),stmt(stmt){}
        QueryRemove(QueryRemove&& other):db(other.db),stmt(other.stmt){other.stmt=nullptr;}
        ~QueryRemove(){ if(stmt)sqlite3_finalize(stmt); }
    };

    // --- Syntax sugar, just condensed versions to simplify single-use queries ---

    template <typename... InArgs>
    std::expected<std::monostate, error_t> execute(std::string_view sql, InArgs&&... args) {
        auto runner = make_execute(sql);
        if (!runner) return std::unexpected{runner.error()};
        return runner->with(std::forward<InArgs>(args)...);
    }

    template <typename Out, typename... InArgs>
    std::expected<std::vector<Out>, error_t> query(std::string_view sql, InArgs&&... args) {
        auto extractor = make_query<Out>(sql);
        if (!extractor) return std::unexpected{extractor.error()};
        return extractor->with(std::forward<InArgs>(args)...);
    }

    template <typename In, typename Out = void>
    std::expected<std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>, error_t> 
    insert(const In& obj, std::string_view table) {
        auto inserter = make_insert<In, Out>(table);
        if (!inserter) return std::unexpected{inserter.error()};
        return inserter->with(obj);
    }

    template <typename In, typename Out = void>
    std::expected<std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>, error_t> 
    update(const In& obj, std::string_view table) {
        auto updater = make_update<In, Out>(table);
        if (!updater) return std::unexpected{updater.error()};
        return updater->with(obj);
    }

    template <typename In, typename Out = void>
    std::expected<std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>, error_t> 
    remove(const In& obj, std::string_view table) {
        auto remover = make_remove<In, Out>(table);
        if (!remover) return std::unexpected{remover.error()};
        return remover->with(obj);
    }

    template <typename Out, typename... InArgs>
    std::expected<std::vector<Out>, error_t> select(std::string_view table, std::string_view where_clause = "", InArgs&&... args) {
        auto selector = make_select<Out>(table, where_clause);
        if (!selector) return std::unexpected{selector.error()};
        return selector->with(std::forward<InArgs>(args)...);
    }
};

}

