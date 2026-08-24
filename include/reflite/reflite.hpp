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
#include <iterator> // Added for std::default_sentinel_t

#include <sqlite3.h>
#include "ctp.hpp"

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
    int (*logger)(const char* str, ...) = nullptr;

    // Access the underlying connection (no ownership transfer).
    sqlite3* raw() const noexcept { return handle; }

    // Type-erased query executors
    template <typename Out = void> struct QueryRaw;
    template <typename In, typename Out = void, bool OrReplace = false> struct QueryInsert;
    template <typename In, typename Out = void> struct QueryUpdate;
    template <typename Out = void> struct QueryRemove;

    // --- Ephemeral View Pattern for zero-allocation results ---
    template <typename QueryT, typename Out>
    struct ResultSet {
        const QueryT* parent;

        struct Iterator {
            const QueryT* parent;
            Out current_row;
            int rc;

            void advance() {
                rc = sqlite3_step(parent->stmt);
                if (rc == SQLITE_ROW) {
                    int col = 0;
                    static constexpr auto members = define_static_array(std::meta::nonstatic_data_members_of(^^Out, std::meta::access_context::unchecked()));
                    template for (constexpr auto mem : members) {
                        constexpr column_t meta = details::get_col_meta(mem);
                        if constexpr (!meta.ignore) {
                            using FieldType = [:remove_cvref(type_of(mem)):];
                            current_row.[:mem:] = details::SqliteTypeMap<FieldType, meta.type>::Extract(parent->stmt, col++);
                        }
                    }
                } else if (rc != SQLITE_DONE) {
                    parent->db->log_error(SQLITE_ERROR, "Iterator step");
                }
            }

            bool operator!=(std::default_sentinel_t) const { return rc == SQLITE_ROW; }
            Iterator& operator++() { advance(); return *this; }
            const Out& operator*() const { return current_row; }
        };

        Iterator begin() const {
            Iterator it{parent, {}, SQLITE_OK};
            it.advance(); // Fetch the first row lazily
            return it;
        }

        std::default_sentinel_t end() const { return {}; }
    };

private:
    sqlite3* handle = nullptr;

    void log_error(int rc, std::string_view context) const {
        if (log_errors && logger!=nullptr) {
            logger("SQLite Error [%s]: {%i} (%s)", context, rc, handle ? sqlite3_errmsg(handle) : "N/A");
        } 
    }

    template <typename In, typename Out, bool OrReplace = false>
    static constexpr std::string insert_strbld(std::string_view table){
        static constexpr auto members = define_static_array(std::meta::nonstatic_data_members_of(^^In, std::meta::access_context::unchecked()));
        std::string sql;
        if constexpr (OrReplace) sql = "INSERT OR REPLACE INTO ";
        else sql = "INSERT INTO ";
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

        return sql;
    }

    template <typename In, typename Out>
    static constexpr std::string update_strbld(std::string_view table, std::string_view where_clause){
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
        
        return sql;
    }

    template <typename Out>
    static constexpr std::string remove_strbld(std::string_view table, std::string_view where_clause){
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

        return sql;
    }

    template <typename Out>
    static constexpr std::string select_strbld(std::string_view table, std::string_view where_clause){
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

        return sql;
    }

public:
    ~Database() { auto _ = deinit(); }

    std::expected<std::monostate, error_t> init(const char* filepath, std::optional<int> flags = {}) {
        int rc = sqlite3_open_v2(filepath, &handle, flags.value_or(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE), nullptr);
        if (rc != SQLITE_OK) {
            log_error(rc, "init");
            return std::unexpected{error_t::GenericError};
        }
        return {};
    }

    std::expected<std::monostate, error_t> deinit() {
        if (handle) {
            int rc = sqlite3_close_v2(handle);
            if (rc != SQLITE_OK) {
                log_error(rc, "deinit");
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
            log_error(rc, "prepare_or_cached");
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

    template <typename Out = void>
    struct Query{
        Database& db;

        Query(Database& db):db(db){}

        template <ctp::Param Sql>
        std::expected<std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>, Database::error_t> 
        run(auto&&... args) {
            auto selector = make<Sql>();
            if (!selector) return std::unexpected{selector.error()};
            return selector->with(std::forward<decltype(args)>(args)...);
        }

        std::expected<std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>, Database::error_t> 
        run(std::string_view sql, auto&&... args) {
            auto selector = make(sql);
            if (!selector) return std::unexpected{selector.error()};
            return selector->with(std::forward<decltype(args)>(args)...);
        }

        template <ctp::Param Sql>
        std::expected<Database::QueryRaw<Out>, Database::error_t> make() {
            auto stmt = db.prepare_or_cached(std::define_static_string(std::string{Sql.value}));
            if (!stmt) return std::unexpected{stmt.error()};
            return Database::QueryRaw<Out>{&db, stmt.value()};
        }

        std::expected<Database::QueryRaw<Out>, Database::error_t> make(std::string_view sql) {
            auto stmt = db.prepare_or_cached(sql);
            if (!stmt) return std::unexpected{stmt.error()};
            return Database::QueryRaw<Out>{&db, stmt.value()};
        }
    };

    template <typename Out = void>
    Query<Out> query(){return Query<Out>(*this);}


    template <typename In, typename Out = void, bool OrReplace = false>
    struct Insert{
        Database& db;

        Insert(Database& db):db(db){}

        template <ctp::Param Table>
        std::expected<std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>, Database::error_t> 
        run(const In& obj) {
            auto selector = make<Table>();
            if (!selector) return std::unexpected{selector.error()};
            return selector->with(obj);
        }

        std::expected<std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>, Database::error_t> 
        run(const In& obj, std::string_view table) {
            auto selector = make(table);
            if (!selector) return std::unexpected{selector.error()};
            return selector->with(obj);
        }

        template <ctp::Param Table>
        std::expected<Database::QueryInsert<In, Out, OrReplace>, Database::error_t> make() {
            auto stmt = db.prepare_or_cached(std::define_static_string(Database::insert_strbld<In, Out, OrReplace>(Table.value)));
            if (!stmt) return std::unexpected{stmt.error()};
            return Database::QueryInsert<In, Out, OrReplace>{&db, stmt.value()};
        }

        std::expected<Database::QueryInsert<In, Out, OrReplace>, Database::error_t> make(std::string_view table) {
            auto stmt = db.prepare_or_cached(Database::insert_strbld<In, Out, OrReplace>(table));
            if (!stmt) return std::unexpected{stmt.error()};
            return Database::QueryInsert<In, Out, OrReplace>{&db, stmt.value()};
        }
    };

    template <typename In, typename Out = void>
    using InsertOrReplace = Insert<In, Out, true>;

    template <typename In, typename Out = void>
    Insert<In, Out> insert(){return Insert<In, Out>(*this);}

    template <typename In, typename Out = void>
    InsertOrReplace<In, Out> insert_or_replace(){return InsertOrReplace<In, Out>(*this);}


    template <typename In, typename Out = void>
    struct Update{
        Database& db;

        Update(Database& db):db(db){}

        template <ctp::Param Table, ctp::Param WhereClause = "">
        std::expected<std::vector<Out>, error_t> run(auto&&... args) {
            auto selector = make<Table, WhereClause>();
            if (!selector) return std::unexpected{selector.error()};
            return selector->with(std::forward<decltype(args)>(args)...);
        }

        std::expected<std::vector<Out>, error_t> run(std::string_view table, std::string_view where_clause = {}, auto&&... args) {
            auto selector = make(table, where_clause);
            if (!selector) return std::unexpected{selector.error()};
            return selector->with(std::forward<decltype(args)>(args)...);
        }

        template <ctp::Param Table, ctp::Param WhereClause = "">
        std::expected<Database::QueryUpdate<In, Out>, Database::error_t> make() {
            auto stmt = db.prepare_or_cached(std::define_static_string(Database::update_strbld<In, Out>(Table.value, WhereClause.value)));
            if (!stmt) return std::unexpected{stmt.error()};
            return Database::QueryUpdate<In, Out>{&db, stmt.value()};
        }

        std::expected<Database::QueryUpdate<In, Out>, Database::error_t> make(std::string_view table, std::string_view where_clause = "") {
            auto stmt = db.prepare_or_cached(Database::update_strbld<In, Out>(table, where_clause));
            if (!stmt) return std::unexpected{stmt.error()};
            return Database::QueryUpdate<In, Out>{&db, stmt.value()};
        }
    };

    template <typename In, typename Out = void>
    Update<In, Out> update(){return Update<In, Out>(*this);}

    template <typename Out = void>
    struct Remove{
        Database& db;

        Remove(Database& db):db(db){}

        template <ctp::Param Table, ctp::Param WhereClause = "">
        std::expected<std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>, Database::error_t> 
        run() {
            auto selector = make<Table, WhereClause>();
            if (!selector) return std::unexpected{selector.error()};
            return selector->with();
        }

        std::expected<std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>, Database::error_t> 
        run(std::string_view table, std::string_view where_clause = {}) {
            auto selector = make(table, where_clause);
            if (!selector) return std::unexpected{selector.error()};
            return selector->with();
        }

        template <ctp::Param Table, ctp::Param WhereClause = "">
        std::expected<Database::QueryRemove<Out>, Database::error_t> make() {
            auto stmt = db.prepare_or_cached(std::define_static_string(Database::remove_strbld<Out>(Table.value, WhereClause.value)));
            if (!stmt) return std::unexpected{stmt.error()};
            return Database::QueryRemove<Out>{&db, stmt.value()};
        }

        std::expected<Database::QueryRemove<Out>, Database::error_t> make(std::string_view table, std::string_view where_clause = "") {
            auto stmt = db.prepare_or_cached(Database::remove_strbld<Out>(table, where_clause));
            if (!stmt) return std::unexpected{stmt.error()};
            return Database::QueryRemove<Out>{&db, stmt.value()};
        }
    };

    template <typename Out = void>
    Remove<Out> remove(){return Remove<Out>(*this);}

    template <typename Out, typename... Args>
    struct Select{
        Database& db;

        Select(Database& db):db(db){}

        template <ctp::Param Table, ctp::Param WhereClause = "">
        std::expected<std::vector<Out>, error_t> run(auto&&... args) {
            auto selector = make<Table, WhereClause>();
            if (!selector) return std::unexpected{selector.error()};
            return selector->with(std::forward<decltype(args)>(args)...);
        }

        std::expected<std::vector<Out>, error_t> run(std::string_view table, std::string_view where_clause = {}, auto&&... args) {
            auto selector = make(table, where_clause);
            if (!selector) return std::unexpected{selector.error()};
            return selector->with(std::forward<decltype(args)>(args)...);
        }

        template <ctp::Param Table, ctp::Param WhereClause = "">
        std::expected<Database::QueryRaw<Out>, Database::error_t> make() {
            auto stmt = db.prepare_or_cached(std::define_static_string(Database::select_strbld<Out>(Table.value, WhereClause.value)));
            if (!stmt) return std::unexpected{stmt.error()};
            return Database::QueryRaw<Out>{&db, stmt.value()};
        }

        std::expected<Database::QueryRaw<Out>, Database::error_t> make(std::string_view table, std::string_view where_clause = "") {
            auto stmt = db.prepare_or_cached(Database::select_strbld<Out>(table, where_clause));
            if (!stmt) return std::unexpected{stmt.error()};
            return Database::QueryRaw<Out>{&db, stmt.value()};
        }
    };

    template <typename Out, typename... Args>
    Select<Out,Args...> select(){return Select<Out,Args...>(*this);}

    // --- Execution wrappers, to later finalize the reusable query ---

    template <typename Out>
    struct QueryRaw {
        private:
        Database* db;
        sqlite3_stmt* stmt;

        using ret_t = std::expected<std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>, Database::error_t>;

        template <typename... InArgs>
        error_t bind(InArgs&&... args) const{
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            int bind_idx = 1;
            bool bind_ok = true;
            (..., (bind_ok = bind_ok && (details::SqliteTypeMap<std::remove_cvref_t<InArgs>>::Bind(stmt, bind_idx++, args) == SQLITE_OK)));
            
            if (!bind_ok) {
                db->log_error(SQLITE_ERROR, "QueryRaw bind");
                return error_t::BindError;
            }
            return error_t::Ok;
        }

        friend struct ResultSet<QueryRaw, Out>::Iterator;

        public:

        template <typename... InArgs>
        std::expected<ResultSet<QueryRaw, Out>, error_t> iterate_with(InArgs&&... args) const requires (!std::is_same_v<Out, void>) {
            error_t ret = bind(std::forward<decltype(args)>(args)...);
            if(ret!=error_t::Ok)return std::unexpected{ret};
            else return ResultSet<QueryRaw, Out>{this};
        }

        ret_t with(auto&&... args) const {
            error_t ret = bind(std::forward<decltype(args)>(args)...);
            if(ret!=error_t::Ok)return std::unexpected{ret};

            if constexpr (std::is_same_v<Out,void>){
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    db->log_error(SQLITE_ERROR, "QueryRaw step");
                    return std::unexpected{error_t::StepError};
                }
                return std::monostate{};
            }
            else{
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
                    db->log_error(SQLITE_ERROR, "QueryRaw step");
                    return std::unexpected{error_t::StepError};
                }
                return results;
            }
        }

        QueryRaw(Database* db, sqlite3_stmt* stmt):db(db),stmt(stmt){}
        QueryRaw(QueryRaw&& other):db(other.db),stmt(other.stmt){other.stmt=nullptr;}
        ~QueryRaw(){ if(stmt)sqlite3_finalize(stmt); }
    };

    template <typename In, typename Out, bool OrReplace>
    struct QueryInsert {
        private:
        Database* db;
        sqlite3_stmt* stmt;

        using ReturnType = std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>;

        friend struct ResultSet<QueryInsert, Out>::Iterator;

        error_t bind(const In& obj) const{
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            int bind_idx = 1;
            static constexpr auto members = define_static_array(std::meta::nonstatic_data_members_of(^^In, std::meta::access_context::unchecked()));
            template for (constexpr auto mem : members) {
                constexpr column_t meta = details::get_col_meta(mem);
                if constexpr (!meta.ignore) {
                    using ValType = std::remove_cvref_t<decltype(obj.[:mem:])>;
                    if (details::SqliteTypeMap<ValType, meta.type>::Bind(stmt, bind_idx++, obj.[:mem:]) != SQLITE_OK) {
                        db->log_error(SQLITE_ERROR, "QueryInsert bind");
                        return error_t::BindError;
                    }
                }
            }
            return error_t::Ok;
        }

        public:

        std::expected<ResultSet<QueryInsert, Out>, error_t> iterate_with(const In& obj) const requires (!std::is_same_v<Out, void>) {
            error_t ret = bind(std::forward<decltype(obj)>(obj));
            if(ret!=error_t::Ok)return std::unexpected{ret};
            else return ResultSet<QueryInsert, Out>{this};
        }

        std::expected<ReturnType, error_t> with(const In& obj) const {
            error_t ret = bind(std::forward<decltype(obj)>(obj));
            if(ret!=error_t::Ok)return std::unexpected{ret};
            
            if constexpr (std::is_same_v<Out, void>) {
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    db->log_error(SQLITE_ERROR, "QueryInsert step");
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
                    db->log_error(SQLITE_ERROR, "QueryInsert step RETURNING");
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
        private:
        Database* db;
        sqlite3_stmt* stmt;

        using ReturnType = std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>;

        friend struct ResultSet<QueryUpdate, Out>::Iterator;

        template <typename... InArgs>
        error_t bind(const In& obj, InArgs&&... args) const{
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            int bind_idx = 1;
            bool bind_ok = true;

            static constexpr auto members = define_static_array(std::meta::nonstatic_data_members_of(^^In, std::meta::access_context::unchecked()));

            template for (constexpr auto mem : members) {
                constexpr column_t meta = details::get_col_meta(mem);
                if constexpr (!meta.ignore) {
                    using ValType = std::remove_cvref_t<decltype(obj.[:mem:])>;
                    if (details::SqliteTypeMap<ValType, meta.type>::Bind(stmt, bind_idx++, obj.[:mem:]) != SQLITE_OK) return error_t::BindError;
                }
            }

            (..., (bind_ok = bind_ok && (details::SqliteTypeMap<std::remove_cvref_t<InArgs>>::Bind(stmt, bind_idx++, args) == SQLITE_OK)));
            if (!bind_ok) return error_t::BindError;
            return error_t::Ok;
        }

        public:
        template <typename... InArgs>
        std::expected<ResultSet<QueryUpdate, Out>, error_t> iterate_with(const In& obj, InArgs&&... args) const requires (!std::is_same_v<Out, void>) {
            error_t ret = bind(obj, std::forward<decltype(args)>(args)...);
            if(ret!=error_t::Ok)return std::unexpected{ret};
            else return ResultSet<QueryUpdate, Out>{this};
        }

        template <typename... InArgs>
        std::expected<ReturnType, error_t> with(const In& obj, InArgs&&... args) const {
            error_t ret = bind(obj, std::forward<decltype(args)>(args)...);
            if(ret!=error_t::Ok)return std::unexpected{ret};

            if constexpr (std::is_same_v<Out, void>) {
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    db->log_error(SQLITE_ERROR, "QueryUpdate step");
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
                    db->log_error(SQLITE_ERROR, "QueryUpdate step RETURNING");
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
        private:
        Database* db;
        sqlite3_stmt* stmt;

        using ReturnType = std::conditional_t<std::is_same_v<Out, void>, std::monostate, std::vector<Out>>;


        template <typename... InArgs>
        error_t bind(InArgs&&... args) const{
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            int bind_idx = 1;
            bool bind_ok = true;
            (..., (bind_ok = bind_ok && (details::SqliteTypeMap<std::remove_cvref_t<InArgs>>::Bind(stmt, bind_idx++, args) == SQLITE_OK)));
            
            if (!bind_ok) {
                db->log_error(SQLITE_ERROR, "QueryRemove bind");
                return error_t::BindError;
            }
            return error_t::Ok;
        }

        friend struct ResultSet<QueryRemove, Out>::Iterator;

        public:
        template <typename... InArgs>
        std::expected<ResultSet<QueryRemove, Out>, error_t> iterate_with(InArgs&&... args) const requires (!std::is_same_v<Out, void>) {
            error_t ret = bind(std::forward<decltype(args)>(args)...);
            if(ret!=error_t::Ok)return std::unexpected{ret};
            else return ResultSet<QueryRemove, Out>{this};
        }

        template <typename... InArgs>
        std::expected<ReturnType, error_t> with(InArgs&&... args) const {
            error_t ret = bind(std::forward<decltype(args)>(args)...);
            if(ret!=error_t::Ok)return std::unexpected{ret};

            if constexpr (std::is_same_v<Out, void>) {
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    db->log_error(SQLITE_ERROR, "QueryRemove step");
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
                    db->log_error(SQLITE_ERROR, "QueryRemove step RETURNING");
                    return std::unexpected{error_t::StepError};
                }
                return results;
            }
        }

        QueryRemove(Database* db, sqlite3_stmt* stmt):db(db),stmt(stmt){}
        QueryRemove(QueryRemove&& other):db(other.db),stmt(other.stmt){other.stmt=nullptr;}
        ~QueryRemove(){ if(stmt)sqlite3_finalize(stmt); }
    };

};

}