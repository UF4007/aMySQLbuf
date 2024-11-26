// Our advanced aMySQLbuf is totally beyond the old Redis.
// Author: UF4007
// 
#pragma once
#define MEM_MYSQL_ON 1
#include <mariadb/mysql.h>
#include "../ioManager/ioManager.h"
#include "../memManager/memManager.h"
#include <mariadb/mysql.h>
#include <unordered_map>
#include <concepts>
#include <ranges>
#include <functional>

namespace asql {
    inline namespace v24a
    {
        template <typename T>
        concept hashable_with_std = requires(T t) {
            { std::hash<T>{}(t) };
            { t == t };
        };

        template <typename T>
        concept hashable_with_asql = requires(T t) {
            { asql_hash(t) } -> std::convertible_to<size_t>;    // index type T has a member function asql_hash()
            { t == t };                                         // index type T overload operator ==
        };

        template <typename T>
        concept hashable = hashable_with_std<T> || hashable_with_asql<T>;

        constexpr size_t default_hash_init_size = 1000;

        //hash function prior: custom->std::hash

        // The first template type is memUnit
        // The second template type is the Primary Index; two rows with the same Primary Index will be identified as the same.
        // The rest of the template types are Auxiliary Index
        struct connect_conf_t
        {
            const char *host;
            const char *user;
            const char *passwd;
            const char *db;
            const char *tablename;
            unsigned int port;
        };
        template <typename TableStruct, typename... Indexs>
            requires std::derived_from<TableStruct, mem::memUnit> && (hashable<Indexs> && ...)
        struct table
        {
            table(mem::memManager *mngr, std::vector<mem::memUnit::mysql_meta> metadata, connect_conf_t connect_conf, std::vector<const char*> index_name, size_t map_size = default_hash_init_size, size_t connect_num = 1);
            table(const table& right) = delete;
            void operator=(const table& right) = delete;
            table(table&& right) = delete;
            void operator=(table &&right) = delete;
            ~table();   // if SQL working threads are not stoped by async_stop(), the deconstruct function will BLOCK current thread!

            using promiseTS = io::coPromise<mem::dumbPtr<TableStruct>>;
            using promiseTSV = io::coPromise<std::vector<mem::dumbPtr<TableStruct>>>;

            //async functions

            // async load the whole table from SQL.
            io::coTask loadAll(io::coPromise<> &prom);

            // insert, always async via SQL, after SQL return, load the new row to each index hashmap
            // The thread ownership of memPtr(row) will borrow to SQL operation thread until promise return. Do not EDIT anyway.
            // if the same primary index exists, the insert will fail.
            io::coTask insert(promiseTS &prom);

            // delete specific ALL rows by index, always async via SQL
            template <typename Index>
            io::coTask deletee(io::coPromise<> &prom, const char *key, const Index &index);

            // update a single row by index, always async via SQL
            // ignore all indexs and readonly variable
            // If more than one row under the specific index has been found, the promise will abort and do nothing with SQL
            // The thread ownership of memPtr(row) will borrow to SQL operation thread until promise return. Do not EDIT anyway.
            template <typename Index>
            io::coTask update(io::coPromise<> &prom, const char *key, const Index &index);

            // select a single row using one index, and use another index to specify the target index, always async via SQL
            // If more than one row under the primary index has been found, the promise will abort and do nothing with SQL
            // The thread ownership of memPtr(row) will borrow to SQL operation thread until promise return. Do not EDIT anyway.
            template <typename Index, typename Index_Alt>
            io::coTask updateIndex(io::coPromise<> &prom, const char *key, const Index &index, const char *key_alt, const Index_Alt &index_alt);

            // select specific FIRST row in loaded map by index, if not found, async select FIRST where index fit via SQL and then load it
            // If given param is not an index in table<>, asycn select via SQL always, and compare with first index(primary index) to judge load or not
            template <typename Index>
            io::coTask select(promiseTS &prom, const char *key, const Index &index);

            // select specific ALL rows in loaded map by index, always async select ALL where index fit via SQL and then load them all
            template <typename Index>
            io::coTask selectAll(promiseTSV &prom, const char *key, const Index &index);

            // reload specific ALL loaded row by index
            template <typename Index>
            io::coTask reload(promiseTS &prom, const char *key, const Index &index);

            // async execute raw instrution via SQL. forced to use stmt.
            // char*, MYSQL_BIND must be accessible until the promise return.
            void rawInstr(io::coPromise<const char*> &prom, MYSQL_BIND *bind_in, MYSQL_BIND *bind_out);

            // async stop all working threads (SQL connections) of this table, this table will be disabled forever.
            void asycn_stop(io::coPromise<> &prom);



            // local functions (sync)

            // select FIRST row by index in loaded hash map only
            template <typename Index>
            mem::dumbPtr<TableStruct> selectMap(const char *key, const Index &index);

            // select ALL row by index in loaded hash map only
            template <typename Index>
            std::pair<
             typename std::unordered_multimap<Index, mem::dumbPtr<TableStruct>>::iterator,
             typename std::unordered_multimap<Index, mem::dumbPtr<TableStruct>>::iterator> selectMapAll(const char *key, const Index &index);

            // make specific ALL loaded row unload
            template <typename Index>
            void unloadMap(const char *key, const Index &index);

            // clean all loaded rows that no one is using, nullptr
            void cleanVacancy();

            inline mem::memManager* getManager() { return *memMngr; }

            inline size_t size() { return tableMap.map.size(); }

        private:
            // our advanced delegate has totally beyond the old std::function
            class Delegate
            {
            private:
                union Param // avoid strict aliasing UB
                {
                    promiseTS promise_ts;
                    io::coPromise<> co_promise;
                    const char *str_ptr;
                    MYSQL_BIND *bind_ptr;
                    MYSQL_STMT **borrow;
                    std::atomic_flag **done;
                    uint64_t raw;

                    inline Param() : raw(0) {}
                    inline ~Param() {}
                };

                enum class FuncType : uint64_t
                {
                    Insert = 0,
                    Delete,
                    Update,
                    Select,
                    LoadAll
                };

                struct Storage
                {
                    FuncType type;
                    Param params[3];
                };

                Storage storage;

                template <typename T>
                inline void store_argument(T &&arg, size_t &index)
                {
                    static_assert(sizeof(T) <= sizeof(uint64_t), "Argument size too large");
                    if constexpr (std::is_same_v<std::decay_t<T>, promiseTS>)
                    {
                        new (&storage.params[index].promise_ts) promiseTS(std::forward<T>(arg));
                    }
                    else if constexpr (std::is_same_v<std::decay_t<T>, io::coPromise<>>)
                    {
                        new (&storage.params[index].co_promise) io::coPromise<>(std::forward<T>(arg));
                    }
                    else
                    {
                        storage.params[index].raw = reinterpret_cast<uint64_t>(std::forward<T>(arg));
                    }
                    ++index;
                }

                template <typename Func>
                static constexpr FuncType get_func_type(Func func)
                {
                    using InsertType = decltype(&table::insert_base);
                    using DeleteType = decltype(&table::delete_base);
                    using UpdateType = decltype(&table::update_base);
                    using SelectType = decltype(&table::select_base);
                    using LoadAllType = decltype(&table::loadall_base);

                    if constexpr (std::is_same_v<Func, InsertType>)
                    {
                        if (func == &table::insert_base)
                            return FuncType::Insert;
                    }
                    else if constexpr (std::is_same_v<Func, DeleteType>)
                    {
                        if (func == &table::delete_base)
                            return FuncType::Delete;
                        if (func == &table::update_base)
                            return FuncType::Update;
                        // same signature between two function pointers.
                    }
                    else if constexpr (std::is_same_v<Func, SelectType>)
                    {
                        if (func == &table::select_base)
                            return FuncType::Select;
                    }
                    else if constexpr (std::is_same_v<Func, LoadAllType>)
                    {
                        if (func == &table::loadall_base)
                            return FuncType::LoadAll;
                    }

                    assert(!"Unknown function type");
                    return FuncType::Insert;
                }

            public:
                template <typename Func, typename... Args>
                    requires std::is_member_function_pointer_v<Func>
                inline Delegate(Func &&func, Args &&...args)
                {
                    static_assert(sizeof...(Args) <= 3, "Too many arguments!");
                    storage.type = get_func_type(func);

                    size_t index = 0;
                    (store_argument(std::forward<Args>(args), index), ...);
                }

                Delegate(Delegate &&right) noexcept
                {
                    std::memcpy(&storage, &right.storage, sizeof(Storage));
                }

                Delegate &operator=(Delegate &&right) noexcept
                {
                    if (this != &right)
                    {
                        std::memcpy(&storage, &right.storage, sizeof(Storage));
                    }
                    return *this;
                }

                Delegate(const Delegate &) = delete;
                Delegate &operator=(const Delegate &) = delete;

                inline void operator()(table<TableStruct, Indexs...> *pthis, MYSQL *my, MYSQL_STMT *stmt)
                {
                    switch (storage.type)
                    {
                    case FuncType::Insert:
                        std::invoke(&table::insert_base, pthis, my, stmt,
                                    storage.params[0].promise_ts,
                                    storage.params[1].bind_ptr);
                        break;
                    case FuncType::Delete:
                        std::invoke(&table::delete_base, pthis, my, stmt,
                                    storage.params[0].co_promise,
                                    storage.params[1].str_ptr,
                                    storage.params[2].bind_ptr);
                        break;
                    case FuncType::Update:
                        std::invoke(&table::update_base, pthis, my, stmt,
                                    storage.params[0].co_promise,
                                    storage.params[1].str_ptr,
                                    storage.params[2].bind_ptr);
                        break;
                    case FuncType::Select:
                        std::invoke(&table::select_base, pthis, my, stmt,
                                    storage.params[0].promise_ts,
                                    storage.params[1].bind_ptr,
                                    storage.params[2].str_ptr);
                        break;
                    case FuncType::LoadAll:
                        std::invoke(&table::loadall_base, pthis, my, stmt,
                                    storage.params[0].co_promise,
                                    storage.params[1].borrow,
                                    storage.params[2].done);
                        break;
                    }
                }

                ~Delegate()
                {
                    // use stack memory, will clean automately.

                    // if (storage.type == FuncType::Insert)
                    // {
                    //     storage.params[0].promise_ts.~promiseTS();
                    // }
                    // else if (storage.type == FuncType::Delete || storage.type == FuncType::Update)
                    // {
                    //     storage.params[0].co_promise.~coPromise();
                    // }
                }
            };

            template <int layer, typename... _Indexs>
            struct table_map_t
            {
                inline table_map_t(size_t, void*, void*) {};
            };

            template <int layer, typename _Index, typename... _Indexs>
            struct table_map_t<layer, _Index, _Indexs...>
            {
                struct _hash
                {
                    inline size_t operator()(const _Index &index) const noexcept
                    {
                        if constexpr (hashable_with_asql<_Index>)
                        {
                            return index.asql_hash();
                        }
                        else if constexpr (hashable_with_std<_Index>)
                        {
                            return std::hash<_Index>{}(index);
                        }
                        else
                        {
                            static_assert(hashable<_Index>, "Index type is not hashable");
                            return 0;
                        }
                    }
                };
                struct _equal
                {
                    inline bool operator()(const _Index &lhs, const _Index &rhs) const noexcept
                    {
                        return lhs == rhs;
                    }
                };

                std::unordered_multimap<_Index, mem::dumbPtr<TableStruct>, _hash, _equal> map;
                table_map_t<layer + 1, _Indexs...> next;
                size_t index_where; //where the metadata this index is
                inline table_map_t(size_t IndexMapInitSize,
                                   std::vector<mem::memUnit::mysql_meta> *metadata,
                                   std::vector<const char *> *index_name) : map(IndexMapInitSize), next(IndexMapInitSize, metadata, index_name)
                {
                    for (auto i = layer; i < metadata->size(); i++)
                    {
                        if ((*index_name)[layer] == (*metadata)[i].key)
                        {
                            //assert((*metadata)[i].readonly == true || !"table init error: all the indexs must be readonly.");
                            index_where = i;
                            return;
                        }
                    }
                    assert(!"table init error: index name mismatch the metadata!");
                };
                inline mem::dumbPtr<TableStruct> load(mem::dumbPtr<TableStruct> &insertee, MYSQL_BIND *bind)
                {
                    MYSQL_BIND &bindi = *(bind + index_where);
                    _Index first;
                    void *firstptr;
                    if constexpr(std::is_same_v<_Index, std::string>)
                    {
                        first.resize(*bindi.length);
                        firstptr = (void *)first.c_str();
                    }
                    else
                    {
                        firstptr = (void *)&first;
                    }
                    memcpy(firstptr, bindi.buffer, bindi.buffer_length);
                    if constexpr (layer == 0)
                    {
                        auto found = map.find(first);
                        if (found != map.end())
                            return found->second;
                    }
                    map.insert(std::pair<_Index, mem::dumbPtr<TableStruct>>(first, insertee));
                    if constexpr (sizeof...(_Indexs) > 0)
                        next.load(insertee, bind);
                    return nullptr;
                }
                inline void unload(mem::dumbPtr<TableStruct> &insertee, MYSQL_BIND *bind)
                {
                    MYSQL_BIND &bindi = *(bind + index_where);
                    _Index first;
                    void *firstptr;
                    if constexpr (std::is_same_v<_Index, std::string>)
                    {
                        first.resize(bindi.buffer_length);
                        firstptr = (void *)first.c_str();
                    }
                    else
                    {
                        firstptr = (void *)&first;
                    }
                    memcpy(firstptr, bindi.buffer, bindi.buffer_length);
                    auto range = map.equal_range(first);
                    for (auto it = range.first; it != range.second; ++it)
                    {
                        if (it->second == insertee)
                        {
                            map.erase(it);
                            break;
                        }
                    }
                    if constexpr (sizeof...(_Indexs) > 0)
                        next.unload(insertee, bind);
                }
                template <typename Index>
                inline void erase(size_t where, const Index &index)
                {
                    if constexpr (std::is_convertible<_Index, Index>::value || std::is_constructible<_Index, Index>::value)
                    {
                        if (where == index_where)
                        {
                            map.erase(index);
                            return;
                        }
                    }
                    if constexpr (sizeof...(_Indexs) > 0)
                        next.erase(where, index);
                    else
                        assert(!"Index ERROR: Index type or name mismatch!");
                }
                template <typename Index>
                inline mem::dumbPtr<TableStruct> select(size_t where, const Index &index)
                {
                    if constexpr (std::is_convertible<_Index, Index>::value || std::is_constructible<_Index, Index>::value)
                    {
                        if (where == index_where)
                        {
                            auto f = map.find(index);
                            if (f != map.end())
                            {
                                return f->second;
                            }
                            else
                            {
                                return nullptr;
                            }
                        }
                    }
                    if constexpr (sizeof...(_Indexs) > 0)
                        return next.select(where, index);
                    else
                        assert(!"Index ERROR: Index type or name mismatch!");
                    return nullptr;
                }
                template <typename Index>
                inline std::pair<
                    typename std::unordered_multimap<Index, mem::dumbPtr<TableStruct>>::iterator,
                    typename std::unordered_multimap<Index, mem::dumbPtr<TableStruct>>::iterator>
                selectAll(size_t where, const Index &index)
                {
                    if constexpr (std::is_convertible<_Index, Index>::value || std::is_constructible<_Index, Index>::value)
                    {
                        if (where == index_where)
                        {
                            return map.equal_range(index);
                        }
                    }
                    if constexpr (sizeof...(_Indexs) > 0)
                        return next.selectAll(where, index);
                    else
                        assert(!"Index ERROR: Index type or name mismatch!");
                    using IteratorType = typename std::unordered_multimap<Index, mem::dumbPtr<TableStruct>>::iterator;
                    return std::make_pair(IteratorType(), IteratorType());
                }
                template <typename Index>
                inline void updateIndex(mem::dumbPtr<TableStruct> ptr, size_t where, Index &index);
            };


            
            std::vector<mem::memUnit::mysql_meta> metadata;
            std::vector<const char *> index_name;
            size_t metadata_write_sum = 0;
            std::string instruction_loadall;
            std::string instruction_insert;
            std::string instruction_delete;
            std::string instruction_update;
            std::string instruction_select;
            std::string instruction_locktable_read;
            std::string instruction_unlocktable;

            mem::dumbPtr<mem::memManager> memMngr;
            table_map_t<0, Indexs...> tableMap;
            std::atomic<int> queue_count;
            io::dualBuffer<std::vector<Delegate>> queue_instr; // LIFO
            static constexpr size_t queue_overload_limit = 100; // The new SQL instruction will always fail if more than 100 instructions are pending.

            void loadall_base(MYSQL *my, MYSQL_STMT *stmt, io::coPromise<> &prom, MYSQL_STMT **borrow, std::atomic_flag **done); // borrow a SQL thread (connect) to process multi-row fetch relative stuff.
            void insert_base(MYSQL *my, MYSQL_STMT *stmt, promiseTS &prom, MYSQL_BIND *bindr);
            void delete_base(MYSQL *my, MYSQL_STMT *stmt, io::coPromise<> &prom, const char *key, MYSQL_BIND *bindr);
            void update_base(MYSQL *my, MYSQL_STMT *stmt, io::coPromise<> &prom, const char *key, MYSQL_BIND *bindr);
            void select_base(MYSQL *my, MYSQL_STMT *stmt, promiseTS &prom, MYSQL_BIND *bindr, const char *key);
            void raw_base(MYSQL *my, MYSQL_STMT *stmt, io::coPromise<const char *> &prom, MYSQL_BIND *bindw, MYSQL_BIND *bindr);

            size_t getMapWhereByKey(const char* key);
            template<typename Index>
            void IndexBind(MYSQL_BIND& bind, Index &index);

            std::atomic_flag thread_stop = ATOMIC_FLAG_INIT;
            void thread_f(connect_conf_t connect_conf);
            std::vector<std::thread> thread_s= {};
        };



        // definitions
#include "internal/definition.h"
    }
};