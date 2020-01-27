// copyright defined in LICENSE.txt

#pragma once

#include "abieos.hpp"
#include <boost/filesystem.hpp>
#include <chain_kv/chain_kv.hpp>
#include <fc/exception/exception.hpp>
#include <rocksdb/db.h>
#include <rocksdb/table.h>

namespace eosio {

inline void check(bool cond, const char* msg) {
    if (!cond)
        throw std::runtime_error(msg);
}

inline void check(bool cond, const std::string& msg) {
    if (!cond)
        throw std::runtime_error(msg);
}

} // namespace eosio

namespace state_history {
namespace rdb {

inline constexpr abieos::name kvram_id{"eosio.kvram"};
inline constexpr abieos::name kvdisk_id{"eosio.kvdisk"};

inline const std::vector<char> undo_stack_prefix{0x40};
inline const std::vector<char> contract_kv_prefix{0x41};

enum class kv_it_stat {
    iterator_ok     = 0,  // Iterator is positioned at a key-value pair
    iterator_erased = -1, // The key-value pair that the iterator used to be positioned at was erased
    iterator_end    = -2, // Iterator is out-of-bounds
};

struct kv_iterator_rocksdb {
    uint32_t&                num_iterators;
    chain_kv::view&          view;
    uint64_t                 contract;
    chain_kv::view::iterator kv_it;

    kv_iterator_rocksdb(uint32_t& num_iterators, chain_kv::view& view, uint64_t contract, const char* prefix, uint32_t size)
        : num_iterators(num_iterators)
        , view{view}
        , contract{contract}
        , kv_it{view, contract, {prefix, size}} {
        ++num_iterators;
    }

    ~kv_iterator_rocksdb() { --num_iterators; }

    bool is_kv_chainbase_context_iterator() const { return false; }
    bool is_kv_rocksdb_context_iterator() const { return true; }

    kv_it_stat kv_it_status() {
        if (kv_it.is_end())
            return kv_it_stat::iterator_end;
        else if (kv_it.is_erased())
            return kv_it_stat::iterator_erased;
        else
            return kv_it_stat::iterator_ok;
    }

    int32_t kv_it_compare(const kv_iterator_rocksdb& rhs) {
        eosio::check(rhs.is_kv_rocksdb_context_iterator(), "Incompatible key-value iterators");
        auto& r = static_cast<const kv_iterator_rocksdb&>(rhs);
        eosio::check(&view == &r.view && contract == r.contract, "Incompatible key-value iterators");
        eosio::check(!kv_it.is_erased(), "Iterator to erased element");
        eosio::check(!r.kv_it.is_erased(), "Iterator to erased element");
        return compare(kv_it, r.kv_it);
    }

    int32_t kv_it_key_compare(const char* key, uint32_t size) {
        eosio::check(!kv_it.is_erased(), "Iterator to erased element");
        return chain_kv::compare_key(kv_it.get_kv(), chain_kv::key_value{{key, size}, {}});
    }

    kv_it_stat kv_it_move_to_end() {
        kv_it.move_to_end();
        return kv_it_stat::iterator_end;
    }

    kv_it_stat kv_it_next() {
        eosio::check(!kv_it.is_erased(), "Iterator to erased element");
        ++kv_it;
        return kv_it_status();
    }

    kv_it_stat kv_it_prev() {
        eosio::check(!kv_it.is_erased(), "Iterator to erased element");
        --kv_it;
        return kv_it_status();
    }

    kv_it_stat kv_it_lower_bound(const char* key, uint32_t size) {
        kv_it.lower_bound(key, size);
        return kv_it_status();
    }

    kv_it_stat kv_it_key(uint32_t offset, char* dest, uint32_t size, uint32_t& actual_size) {
        eosio::check(!kv_it.is_erased(), "Iterator to erased element");

        std::optional<chain_kv::key_value> kv;
        kv = kv_it.get_kv();

        if (kv) {
            if (offset < kv->key.size())
                memcpy(dest, kv->key.data() + offset, std::min((size_t)size, kv->key.size() - offset));
            actual_size = kv->key.size();
            return kv_it_stat::iterator_ok;
        } else {
            actual_size = 0;
            return kv_it_stat::iterator_end;
        }
    }

    kv_it_stat kv_it_value(uint32_t offset, char* dest, uint32_t size, uint32_t& actual_size) {
        eosio::check(!kv_it.is_erased(), "Iterator to erased element");

        std::optional<chain_kv::key_value> kv;
        kv = kv_it.get_kv();

        if (kv) {
            if (offset < kv->value.size())
                memcpy(dest, kv->value.data() + offset, std::min((size_t)size, kv->value.size() - offset));
            actual_size = kv->value.size();
            return kv_it_stat::iterator_ok;
        } else {
            actual_size = 0;
            return kv_it_stat::iterator_end;
        }
    }
}; // kv_iterator_rocksdb

struct kv_database_config {
    std::uint32_t max_key_size   = 1024;
    std::uint32_t max_value_size = 256 * 1024; // Large enough to hold most contracts
    std::uint32_t max_iterators  = 1024;
};

struct kv_context_rocksdb {
    chain_kv::database&                      database;
    chain_kv::write_session&                 write_session;
    abieos::name                             database_id;
    chain_kv::view                           view;
    abieos::name                             receiver;
    const kv_database_config&                limits;
    uint32_t                                 num_iterators = 0;
    std::shared_ptr<const std::vector<char>> temp_data_buffer;

    kv_context_rocksdb(
        chain_kv::database& database, chain_kv::write_session& write_session, abieos::name database_id, abieos::name receiver,
        const kv_database_config& limits)
        : database{database}
        , write_session{write_session}
        , database_id{database_id}
        , view{write_session, make_prefix()}
        , receiver{receiver}
        , limits{limits} {}

    std::vector<char> make_prefix() {
        std::vector<char> prefix = contract_kv_prefix;
        chain_kv::append_key(prefix, database_id.value);
        return prefix;
    }

    void kv_erase(uint64_t contract, const char* key, uint32_t key_size) {
        eosio::check(abieos::name{contract} == receiver, "Can not write to this key");
        temp_data_buffer = nullptr;
        view.erase(contract, {key, key_size});
    }

    void kv_set(uint64_t contract, const char* key, uint32_t key_size, const char* value, uint32_t value_size) {
        eosio::check(abieos::name{contract} == receiver, "Can not write to this key");
        eosio::check(key_size <= limits.max_key_size, "Key too large");
        eosio::check(value_size <= limits.max_value_size, "Value too large");
        temp_data_buffer = nullptr;
        view.set(contract, {key, key_size}, {value, value_size});
    }

    bool kv_get(uint64_t contract, const char* key, uint32_t key_size, uint32_t& value_size) {
        temp_data_buffer = view.get(contract, {key, key_size});
        if (temp_data_buffer) {
            value_size = temp_data_buffer->size();
            return true;
        } else {
            value_size = 0;
            return false;
        }
    }

    uint32_t kv_get_data(uint32_t offset, char* data, uint32_t data_size) {
        const char* temp      = nullptr;
        uint32_t    temp_size = 0;
        if (temp_data_buffer) {
            temp      = temp_data_buffer->data();
            temp_size = temp_data_buffer->size();
        }
        if (offset < temp_size)
            memcpy(data, temp + offset, std::min(data_size, temp_size - offset));
        return temp_size;
    }

    std::unique_ptr<kv_iterator_rocksdb> kv_it_create(uint64_t contract, const char* prefix, uint32_t size) {
        eosio::check(num_iterators < limits.max_iterators, "Too many iterators");
        return std::make_unique<kv_iterator_rocksdb>(num_iterators, view, contract, prefix, size);
    }
}; // kv_context_rocksdb

struct db_view_state {
    abieos::name                                      receiver;
    chain_kv::database&                               database;
    const kv_database_config                          limits;
    kv_context_rocksdb                                kv_ram;
    kv_context_rocksdb                                kv_disk;
    std::vector<std::unique_ptr<kv_iterator_rocksdb>> kv_iterators;
    std::vector<size_t>                               kv_destroyed_iterators;

    db_view_state(abieos::name receiver, chain_kv::database& database, chain_kv::write_session& write_session)
        : receiver{receiver}
        , database{database}
        , kv_ram{database, write_session, kvram_id, receiver, limits}
        , kv_disk{database, write_session, kvdisk_id, receiver, limits}
        , kv_iterators(1) {}

    void reset() {
        eosio::check(kv_iterators.size() == kv_destroyed_iterators.size() + 1, "iterators are still alive");
        kv_iterators.resize(1);
        kv_destroyed_iterators.clear();
    }
};

template <typename Derived>
struct db_callbacks {
    Derived& derived() { return static_cast<Derived&>(*this); }

    void kv_erase(uint64_t db, uint64_t contract, const char* key, uint32_t key_size) {
        derived().check_bounds(key, key_size);
        return kv_get_db(db).kv_erase(contract, key, key_size);
    }

    void kv_set(uint64_t db, uint64_t contract, const char* key, uint32_t key_size, const char* value, uint32_t value_size) {
        derived().check_bounds(key, key_size);
        derived().check_bounds(value, value_size);
        return kv_get_db(db).kv_set(contract, key, key_size, value, value_size);
    }

    bool kv_get(uint64_t db, uint64_t contract, const char* key, uint32_t key_size, uint32_t& value_size) {
        derived().check_bounds(key, key_size);
        return kv_get_db(db).kv_get(contract, key, key_size, value_size);
    }

    uint32_t kv_get_data(uint64_t db, uint32_t offset, char* data, uint32_t data_size) {
        derived().check_bounds(data, data_size);
        return kv_get_db(db).kv_get_data(offset, data, data_size);
    }

    uint32_t kv_it_create(uint64_t db, uint64_t contract, const char* prefix, uint32_t size) {
        derived().check_bounds(prefix, size);
        auto&    kdb = kv_get_db(db);
        uint32_t itr;
        if (!derived().state.kv_destroyed_iterators.empty()) {
            itr = derived().state.kv_destroyed_iterators.back();
            derived().state.kv_destroyed_iterators.pop_back();
        } else {
            // Sanity check in case the per-database limits are set poorly
            eosio::check(derived().state.kv_iterators.size() <= 0xFFFFFFFFu, "Too many iterators");
            itr = derived().state.kv_iterators.size();
            derived().state.kv_iterators.emplace_back();
        }
        derived().state.kv_iterators[itr] = kdb.kv_it_create(contract, prefix, size);
        return itr;
    }

    void kv_it_destroy(uint32_t itr) {
        kv_check_iterator(itr);
        derived().state.kv_destroyed_iterators.push_back(itr);
        derived().state.kv_iterators[itr].reset();
    }

    int32_t kv_it_status(uint32_t itr) {
        kv_check_iterator(itr);
        return static_cast<int32_t>(derived().state.kv_iterators[itr]->kv_it_status());
    }

    int32_t kv_it_compare(uint32_t itr_a, uint32_t itr_b) {
        kv_check_iterator(itr_a);
        kv_check_iterator(itr_b);
        return derived().state.kv_iterators[itr_a]->kv_it_compare(*derived().state.kv_iterators[itr_b]);
    }

    int32_t kv_it_key_compare(uint32_t itr, const char* key, uint32_t size) {
        derived().check_bounds(key, size);
        kv_check_iterator(itr);
        return derived().state.kv_iterators[itr]->kv_it_key_compare(key, size);
    }

    int32_t kv_it_move_to_end(uint32_t itr) {
        kv_check_iterator(itr);
        return static_cast<int32_t>(derived().state.kv_iterators[itr]->kv_it_move_to_end());
    }

    int32_t kv_it_next(uint32_t itr) {
        kv_check_iterator(itr);
        return static_cast<int32_t>(derived().state.kv_iterators[itr]->kv_it_next());
    }

    int32_t kv_it_prev(uint32_t itr) {
        kv_check_iterator(itr);
        return static_cast<int32_t>(derived().state.kv_iterators[itr]->kv_it_prev());
    }

    int32_t kv_it_lower_bound(uint32_t itr, const char* key, uint32_t size) {
        derived().check_bounds(key, size);
        kv_check_iterator(itr);
        return static_cast<int32_t>(derived().state.kv_iterators[itr]->kv_it_lower_bound(key, size));
    }

    int32_t kv_it_key(uint32_t itr, uint32_t offset, char* dest, uint32_t size, uint32_t& actual_size) {
        derived().check_bounds(dest, size);
        kv_check_iterator(itr);
        return static_cast<int32_t>(derived().state.kv_iterators[itr]->kv_it_key(offset, dest, size, actual_size));
    }

    int32_t kv_it_value(uint32_t itr, uint32_t offset, char* dest, uint32_t size, uint32_t& actual_size) {
        derived().check_bounds(dest, size);
        kv_check_iterator(itr);
        return static_cast<int32_t>(derived().state.kv_iterators[itr]->kv_it_value(offset, dest, size, actual_size));
    }

    kv_context_rocksdb& kv_get_db(uint64_t db) {
        if (db == kvram_id.value)
            return derived().state.kv_ram;
        else if (db == kvdisk_id.value)
            return derived().state.kv_disk;
        throw std::runtime_error("Bad key-value database ID");
    }

    void kv_check_iterator(uint32_t itr) {
        eosio::check(itr < derived().state.kv_iterators.size() && derived().state.kv_iterators[itr], "Bad key-value iterator");
    }

    template <typename Rft, typename Allocator>
    static void register_callbacks() {
        Rft::template add<Derived, &Derived::kv_erase, Allocator>("env", "kv_erase");
        Rft::template add<Derived, &Derived::kv_set, Allocator>("env", "kv_set");
        Rft::template add<Derived, &Derived::kv_get, Allocator>("env", "kv_get");
        Rft::template add<Derived, &Derived::kv_get_data, Allocator>("env", "kv_get_data");
        Rft::template add<Derived, &Derived::kv_it_create, Allocator>("env", "kv_it_create");
        Rft::template add<Derived, &Derived::kv_it_destroy, Allocator>("env", "kv_it_destroy");
        Rft::template add<Derived, &Derived::kv_it_status, Allocator>("env", "kv_it_status");
        Rft::template add<Derived, &Derived::kv_it_compare, Allocator>("env", "kv_it_compare");
        Rft::template add<Derived, &Derived::kv_it_key_compare, Allocator>("env", "kv_it_key_compare");
        Rft::template add<Derived, &Derived::kv_it_move_to_end, Allocator>("env", "kv_it_move_to_end");
        Rft::template add<Derived, &Derived::kv_it_next, Allocator>("env", "kv_it_next");
        Rft::template add<Derived, &Derived::kv_it_prev, Allocator>("env", "kv_it_prev");
        Rft::template add<Derived, &Derived::kv_it_lower_bound, Allocator>("env", "kv_it_lower_bound");
        Rft::template add<Derived, &Derived::kv_it_key, Allocator>("env", "kv_it_key");
        Rft::template add<Derived, &Derived::kv_it_value, Allocator>("env", "kv_it_value");
    }
}; // db_callbacks

class kv_environment : public db_callbacks<kv_environment> {
  public:
    using base = db_callbacks<kv_environment>;
    db_view_state& state;

    kv_environment(db_view_state& state)
        : state{state} {}
    kv_environment(const kv_environment&) = default;

    void check_bounds(const char*, uint32_t) {}

    void kv_set(uint64_t db, uint64_t contract, const std::vector<char>& k, const std::vector<char>& v) {
        base::kv_set(db, contract, k.data(), k.size(), v.data(), v.size());
    }
};

} // namespace rdb
} // namespace state_history
