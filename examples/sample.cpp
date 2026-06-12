//g++ -std=c++26 -O2 -freflection sample.cpp -o ./sample -lsqlite3 -I../include
#include <reflite/reflite.hpp>

#include <print>

using namespace reflite;

struct AssetData {
    [[=sql{.type = db_type_t::Auto}]] int id;
    [[=sql{.type = db_type_t::Text, .name = "file_path"}]] std::string filepath;
    //If basic compatible types are used, no need to manually define the decorators.
    size_t offset;
};

struct AssetInsert {
    [[=sql{.type = db_type_t::Text, .name = "file_path"}]] std::string_view filepath;
    size_t offset;
};

struct AssetUpdate {
    [[=sql{.type = db_type_t::Text, .name = "file_path"}]] std::string_view filepath;
    size_t offset;
};

auto logger (const char* pattern, ...) -> int {
    va_list args;
    va_start(args, pattern);
    int rc = vfprintf(stderr, pattern, args);
    va_end(args);
    return rc;
};

int main() {
    Database db;
    db.logger = logger;

    if (!db.init("engine.db").has_value()) return 1;
    
    auto ex = db.query().run("CREATE TABLE IF NOT EXISTS assets(id INTEGER PRIMARY KEY, file_path TEXT NOT NULL, offset INT NOT NULL)");
    if (!ex) return 1;
    
    {
        // Standard insert + Connection-level diagnostics (Void return, runs immediately)
        AssetInsert new_asset{ .filepath = "textures/wall.png", .offset = 1024 };
        auto err1 = db.insert<AssetInsert>().run<"assets">(new_asset);
        if (err1) std::println("Inserted standard record. ID: {}", db.last_insert_id());

        // Insert + RETURNING clause (Using the zero-allocation view pattern)
        auto inserter = db.insert<AssetInsert, AssetData>().make<"assets">().value();
        if (auto res = inserter.iterate_with(AssetInsert{ .filepath = "textures/wall2.png", .offset = 2048})) {
            for (const auto& row : *res) {
                std::println("Inserted via RETURNING. Generated ID: {} | Offset: {}", row.id, row.offset);
            }
        }

        // Update with RETURNING structural builder (Using the zero-allocation view pattern)
        auto updater = db.update<AssetUpdate, AssetData>().make<"assets", "WHERE id = ?">().value();
        if (auto res = updater.iterate_with({.filepath="NOOO2",.offset=112}, 1)) {
            for (const auto& row : *res) {
                std::println("Update executed. Row modified ID: {}", row.id);
            }
        }
    }

    {
        AssetInsert new_asset{ .filepath = "textures/wall.png", .offset = 1024 };
        auto err1 = db.insert<AssetInsert>().run<"assets">(new_asset);
        err1 = db.insert<AssetInsert>().run<"assets">(AssetInsert{ .filepath = "textures/wall2.png", .offset = 1024});

        // Reusable Queries
        auto updater  = db.update<AssetUpdate>().make<"assets", "WHERE id = ?">().value();
        auto selector = Database::Select<AssetData>{db}.make<"assets">().value();
        auto custom_q = db.query<AssetData>().make<"SELECT id, file_path, offset FROM assets WHERE offset > ?">().value();
        auto delete_q = db.remove<AssetData>().make<"assets", "">().value();

        auto ret_selector = Database::Select<AssetData>{db}.run<"assets">();
        auto ret_selector2 = db.select<AssetData>().run<"assets">();

        //Just commented out or it will delete all content in the table which is not very interesting visually.
        //Add a where clause if you want to scope the operation.
        //delete_q.with();

        // Execution reusing a previously defined query for an UPDATE WITHOUT a return value (fallback to .with())
        auto ret = updater.with({.filepath="NOOO",.offset=112},(int)10);
        std::print("{}\n",(int)ret.error_or(Database::error_t::Ok));

        // SELECT query utilizing zero allocations
        if(auto results = selector.iterate_with()) {
            for (const auto& a : *results) {
                std::println("Updated - ID: {} | File: {} | Offset: {}", a.id, a.filepath, a.offset);
            }
        }

        // Custom Parameterized query utilizing zero allocations
        if (auto large_offsets = custom_q.iterate_with(1500)) {
            for(const auto& a : *large_offsets) {
                std::println("Found large offset - ID: {}", a.id);
            }
        }
    }

    return 0;
}