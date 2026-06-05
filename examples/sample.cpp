//g++ -std=c++26 -O2 -freflection sample.cpp -o ./sample -lsqlite3
#include "../reflite.hpp"

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

int main() {
    Database db;
    if (!db.init("engine.db").has_value()) return 1;
    
    auto ex = db.query().run("CREATE TABLE IF NOT EXISTS assets(id INTEGER PRIMARY KEY, file_path TEXT NOT NULL, offset INT NOT NULL)");
    if (!ex) return 1;
    
    {
        // Standard insert + Connection-level diagnostics
        AssetInsert new_asset{ .filepath = "textures/wall.png", .offset = 1024 };
        auto err1 = db.insert<AssetInsert>().run<"assets">(new_asset);
        if (err1) std::println("Inserted standard record. ID: {}", db.last_insert_id());

        // Insert + RETURNING clause
        auto ret_data = db.insert<AssetInsert, AssetData>().run<"assets">(
            AssetInsert{ .filepath = "textures/wall2.png", .offset = 2048}
        );
        if (ret_data && !ret_data->empty()) {
            std::println("Inserted via RETURNING. Generated ID: {} | Offset: {}", 
                        ret_data->front().id, ret_data->front().offset);
        }

        // Update with RETURNING structural builder
        auto updater = db.update<AssetUpdate, AssetData>().make<"assets", "WHERE id = ?">().value();
        auto updated_rows = updater.with({.filepath="NOOO2",.offset=112}, 1);
        
        if (updated_rows && !updated_rows->empty()) {
            std::println("Update executed. Rows modified: {}", updated_rows->size());
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

        // Execution reusing a previously defined query
        auto ret = updater.with({.filepath="NOOO",.offset=112},(int)10);
        std::print("{}\n",(int)ret.error_or(Database::error_t::Ok));

        auto results = selector.with();
        if(results) {
            for (const auto& a : *results) {
                std::println("Updated - ID: {} | File: {} | Offset: {}", a.id, a.filepath, a.offset);
            }
        }

        auto large_offsets = custom_q.with(1500);
        std::println("Found {} assets with large offsets.", large_offsets ? large_offsets->size() : 0);
    }

    return 0;
}
