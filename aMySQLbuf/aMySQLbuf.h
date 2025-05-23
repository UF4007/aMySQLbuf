// Our advanced aMySQLbuf is totally beyond the old Redis.
// Author: UF4007
// 
#pragma once
#define MEM_MYSQL_ON 1
#include <mariadb/mysql.h>
#include "../ioManager/ioManager.h"
#include "../ebManager/ebManager.h"
#include <unordered_map>
#include <concepts>
#include <ranges>
#include <functional>

struct asql_unused {};
bool asql_cmp(asql_unused a, asql_unused b) { return false; }
size_t asql_hash(asql_unused a) { return 0; }

namespace asql {
    inline namespace v24a
    {
        template <typename T>
        concept cmp_with_operator = requires(const T &a, const T &b) {
            { a == b } -> std::convertible_to<bool>;
        };

        template <typename T>
        concept cmp_with_asql = requires(const T &a, const T &b) {
            { ::asql_cmp(a, b) } -> std::convertible_to<bool>;
        };

        template <typename T>
        concept hashable_with_std = requires(const T &t) {
            { std::hash<T>{}(t) };
        };

        template <typename T>
        concept hashable_with_asql = requires(const T &t) {
            { ::asql_hash(t) } -> std::convertible_to<size_t>;    // index type T has a member function asql_hash()
        };

        template <typename T>
        //concept hashable = (hashable_with_std<T> || hashable_with_asql<T>) && (cmp_with_operator<T> || cmp_with_asql<T>);
        concept hashable = true;

        constexpr size_t default_hash_init_size = 1000;

        // hash function prior: custom asql_hash -> std::hash
        // compare function prior: custom asql_cmp -> operator==



        // The first template type is eb::base
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
            requires std::derived_from<TableStruct, eb::base> && (hashable<Indexs> && ...)
        struct table
        {
            table(eb::manager *mngr, std::vector<eb::base::mysql_meta> metadata, connect_conf_t connect_conf, std::vector<const char *> index_name, size_t map_size = default_hash_init_size, size_t connect_num = 1);
            table(const table& right) = delete;
            void operator=(const table& right) = delete;
            table(table&& right) = delete;
            void operator=(table &&right) = delete;
            ~table();   // if SQL working threads are not stoped by async_stop(), the deconstruct function will BLOCK current thread!

            using futureTS = io::future_with<eb::dumbPtr<TableStruct>>;
            using futureTSV = io::future_with<std::vector<eb::dumbPtr<TableStruct>>>;
            template<typename First, typename... Then>
            struct primary_type{
                using type = First;
            };
            using primary_key_t = primary_type<Indexs...>::type;
            template <typename _Index>
            struct _hash
            {
                inline size_t operator()(const _Index &index) const
                {
                    if constexpr (hashable_with_asql<_Index>)
                    {
                        return ::asql_hash(index);
                    }
                    else if constexpr (hashable_with_std<_Index>)
                    {
                        return std::hash<_Index>{}(index);
                    }
                    else
                    {
                        assert(!"Index type is not hashable");
                        return 0;
                    }
                }
            };
            template <typename _Index>
            struct _equal
            {
                inline bool operator()(const _Index &lhs, const _Index &rhs) const
                {
                    if constexpr (cmp_with_asql<_Index>)
                    {
                        return ::asql_cmp(lhs, rhs);
                    }
                    else if constexpr (cmp_with_operator<_Index>)
                    {
                        return lhs == rhs;
                    }
                    else
                    {
                        assert(!"Index type is not compareable");
                        return 0;
                    }
                }
            };

            //async functions, not thread safe.

            // async load the whole table from SQL.
            template <typename T_FSM>
            void loadAll(io::fsm<T_FSM> &fsm, io::future &prom);

            // insert, always async via SQL, after SQL return, load the new row to each index hashmap
            // The thread ownership of memPtr(row) will borrow to SQL operation thread until promise return. Do not EDIT anyway.
            // if the same primary index exists, the insert will fail.
            template <typename T_FSM>
            void insert(io::fsm<T_FSM> &fsm,  io::future &prom, eb::dumbPtr<TableStruct> &pointer);

            // delete specific ALL rows by index, always async via SQL
            template <typename Index, typename T_FSM>
            void deletee(io::fsm<T_FSM> &fsm,  io::future &prom, const char *key, const Index &index);

            // update a single row by primary index, always async via SQL
            // ignore the primary index and all the readonly variables
            // The thread ownership of memPtr(row) will borrow to SQL operation thread until promise return. Do not EDIT anyway.
            template <typename Index, typename T_FSM>
            void update(io::fsm<T_FSM> &fsm, io::future &prom, const Index &primary_index);

            // select specific FIRST row by index, if not found, async select FIRST where index fit via SQL and then load it
            // If given param is not an index in table<>, asycn select via SQL always, and compare with first index(primary index) to distinguish two rows
            template <typename Index, typename T_FSM>
            void select(io::fsm<T_FSM> &fsm, futureTS &prom, const char *key, const Index &index);

            // select specific ALL rows by index, always async select ALL where index fit via SQL and then load them all
            // If given param is not an index in table<>, asycn select via SQL always, and compare with first index(primary index) to distinguish two rows
            template <typename Index, typename T_FSM>
            void selectAll(io::fsm<T_FSM> &fsm,  futureTSV &prom, const char *key, const Index &index);

            // select specific ALL ranged rows by index, always async select ALL where index fit via SQL and then load them all
            // If given param is not an index in table<>, asycn select via SQL always, and compare with first index(primary index) to judge load or not
            template <typename Index, typename T_FSM>
            void selectRange(io::fsm<T_FSM> &fsm,  futureTSV &prom, std::pair<std::tuple<const char *, const Index &>, std::tuple<const char *, const Index &>> range);

            // reload specific ALL loaded row by index
            template <typename Index, typename T_FSM>
            void reload(io::fsm<T_FSM> &fsm, io::future &prom, const char *key, const Index &index);

            // async execute raw instrution via SQL. forced to use stmt.
            // char*, MYSQL_BIND must be accessible until the promise return.
            template <typename T_FSM>
            void rawInstr(io::fsm<T_FSM> &fsm, io::future_with<const char *> &prom, MYSQL_BIND *bind_in, MYSQL_BIND *bind_out);

            // async stop all working threads (SQL connections) of this table, this table will be disabled forever.
            template <typename T_FSM>
            void asycn_stop(io::fsm<T_FSM> &fsm,  io::future &prom);



            // local functions (sync)

            // designate the Auxiliary index which value will be changed, and needs to reposition within the table build-in hash map
            // the value of the Primary index cannot be change
            // this function will not change the index value within the ptr Struct
            template <typename Index, typename Index_alt>
            void relocateIndexLocal(eb::dumbPtr<TableStruct> &ptr, const char *key, const Index &old_value, const Index_alt &new_value);

            // select FIRST row by index in loaded hash map only
            template <typename Index>
            eb::dumbPtr<TableStruct> selectLocal(const char *key, const Index &index);

            // select ALL row by index in loaded hash map only
            template <typename Index>
            auto selectLocalAll(const char *key, const Index &index);

            // make specific ALL loaded row unload
            template <typename Index>
            void unloadLocal(const char *key, const Index &index);

            // clean all loaded rows
            void clear();

            // clean all loaded rows that no one is using, nullptr
            void cleanVacancy();

            inline eb::manager* getManager() { return *memMngr; }

            inline size_t size() { return tableMap.map.size(); }

        private:
            struct async_promise_with_ptr{
                io::async_promise prom;
                eb::dumbPtr<TableStruct> ptr;
            };
            struct _borrow_para;
            // our advanced delegate has totally beyond the old std::function
            class Delegate
            {
            private:
                union Param // avoid strict aliasing UB
                {
                    async_promise_with_ptr* promise_ts;
                    io::async_promise* co_promise;
                    const char *str_ptr;
                    MYSQL_BIND *bind_ptr;
                    _borrow_para *borrow;
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
                    Borrow
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
                    if constexpr (std::is_same_v<std::decay_t<T>, async_promise_with_ptr*>)
                    {
                        new (&storage.params[index].promise_ts) async_promise_with_ptr*(std::forward<T>(arg));
                    }
                    else if constexpr (std::is_same_v<std::decay_t<T>, io::async_promise*>)
                    {
                        new (&storage.params[index].co_promise) io::async_promise*(std::forward<T>(arg));
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
                    using BorrowType = decltype(&table::genBorrow_base);

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
                    else if constexpr (std::is_same_v<Func, BorrowType>)
                    {
                        if (func == &table::genBorrow_base)
                            return FuncType::Borrow;
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
                    case FuncType::Borrow:
                        std::invoke(&table::genBorrow_base, pthis, my, stmt,
                                    storage.params[0].co_promise,
                                    storage.params[1].borrow);
                        break;
                    }
                }

                ~Delegate()
                {
                    // use stack memory, will clean automately.

                    // if (storage.type == FuncType::Insert)
                    // {
                    //     storage.params[0].promise_ts.~futureTS();
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

                std::unordered_multimap<_Index, eb::dumbPtr<TableStruct>, _hash<_Index>, _equal<_Index>> map;
                table_map_t<layer + 1, _Indexs...> next;
                size_t index_where; //where the metadata this index is
                inline table_map_t(size_t IndexMapInitSize,
                                   std::vector<eb::base::mysql_meta> *metadata,
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
                // if an existing struct (distinguished as the same primary index) was found, return the pointer of the existing struct, otherwise return nullptr.
                inline eb::dumbPtr<TableStruct> load(eb::dumbPtr<TableStruct> &insertee, MYSQL_BIND *bind)
                {
                    MYSQL_BIND &bindi = *(bind + index_where);
                    std::pair<_Index, eb::dumbPtr<TableStruct>> newPair;
                    _Index &first = (_Index &)newPair.first;
                    uint8_t *firstptr;
                    if constexpr(std::is_same_v<_Index, std::string>)
                    {
                        first.resize(*bindi.length);
                        firstptr = (uint8_t *)first.data();
                    }
                    else
                    {
                        firstptr = (uint8_t *)&first;
                    }
                    memcpy(firstptr, bindi.buffer, bindi.buffer_length);
                    if constexpr (layer == 0)
                    {
                        auto found = map.find(first);
                        if (found != map.end())
                            return found->second;
                    }
                    newPair.second = insertee;
                    map.insert(reinterpret_cast<std::pair<const _Index, eb::dumbPtr<TableStruct>> &>(newPair));
                    if constexpr (sizeof...(_Indexs) > 0)
                        return next.load(insertee, bind);
                    return nullptr;
                }
                inline void unload(eb::dumbPtr<TableStruct> &insertee, MYSQL_BIND *bind)
                {
                    MYSQL_BIND &bindi = *(bind + index_where);
                    _Index first;
                    uint8_t *firstptr;
                    if constexpr (std::is_same_v<_Index, std::string>)
                    {
                        first.resize(bindi.buffer_length);
                        firstptr = (uint8_t *)first.data();
                    }
                    else
                    {
                        firstptr = (uint8_t *)&first;
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
                    if constexpr (std::is_same<_Index, Index>::value || std::is_convertible<_Index, Index>::value || std::is_constructible<_Index, Index>::value)
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
                inline eb::dumbPtr<TableStruct> select(size_t where, const Index &index)
                {
                    if constexpr (std::is_same<_Index, Index>::value || std::is_convertible<_Index, Index>::value || std::is_constructible<_Index, Index>::value)
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
                    typename std::unordered_multimap<Index, eb::dumbPtr<TableStruct>, _hash<Index>, _equal<Index>>::iterator,
                    typename std::unordered_multimap<Index, eb::dumbPtr<TableStruct>, _hash<Index>, _equal<Index>>::iterator>
                selectAll(size_t where, const Index &index)
                {
                    using IteratorType = typename std::unordered_multimap<Index, eb::dumbPtr<TableStruct>, _hash<Index>, _equal<Index>>::iterator;
                    if constexpr (std::is_same<_Index, Index>::value || std::is_convertible<_Index, Index>::value || std::is_constructible<_Index, Index>::value)
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
                    return std::make_pair(IteratorType(), IteratorType());
                }
                inline void clear()
                {
                    map.clear();
                    if constexpr (sizeof...(_Indexs) > 0)
                        next.clear();
                }
                template <typename Index, typename Index_alt>
                inline void relocateIndex(eb::dumbPtr<TableStruct> &ptr, size_t where, const Index &old_value, const Index_alt &new_value)
                {
                    if constexpr ((std::is_same<_Index, Index>::value || std::is_convertible<_Index, Index>::value || std::is_constructible<_Index, Index>::value) &&
                                  (std::is_same<_Index, Index_alt>::value || std::is_convertible<_Index, Index_alt>::value || std::is_constructible<_Index, Index_alt>::value))
                    {
                        if (where == index_where)
                        {
                            auto range = map.equal_range(old_value);
                            for (auto it = range.first; it != range.second; ++it)
                            {
                                if (it->second == ptr)
                                {
                                    //old value has been found, and erase
                                    map.erase(it);
                                    break;
                                }
                            }
                            // re-insert
                            std::pair<_Index, eb::dumbPtr<TableStruct>> newPair;
                            if constexpr (std::is_array_v<_Index> && std::is_array_v<Index_alt>)
                            {
                                memcpy(&newPair.first, &new_value, sizeof(_Index));
                            }
                            else
                            {
                                newPair.first = new_value;
                            }
                            map.insert(reinterpret_cast<std::pair<const _Index, eb::dumbPtr<TableStruct>> &>(newPair));
                            return;
                        }
                    }
                    if constexpr (sizeof...(_Indexs) > 0)
                        return next.relocateIndex(ptr, where, old_value, new_value);
                    else
                        assert(!"Index ERROR: Index type or name mismatch!");
                }
            };


            
            std::vector<eb::base::mysql_meta> metadata;
            std::vector<const char *> index_name;
            size_t metadata_write_sum = 0;
            std::string instruction_loadall;
            std::string instruction_loadall2;
            std::string instruction_insert;
            std::string instruction_delete;
            std::string instruction_update;
            std::string instruction_select;
            std::string instruction_locktable_read;
            std::string instruction_unlocktable;

            eb::dumbPtr<eb::manager> memMngr;
            table_map_t<0, Indexs...> tableMap;
            std::atomic<int> queue_count;
            io::dualbuf<std::vector<Delegate>> queue_instr; // LIFO
            static constexpr size_t queue_overload_limit = 100; // The new SQL instruction will always fail if more than 100 instructions are pending.

            struct _borrow_para
            {
                MYSQL_BIND *bind_in;
                MYSQL_STMT **borrow;
                std::atomic_flag **done;
                const char* instr;
                size_t instr_len;
            };
            void genBorrow_base(MYSQL *my, MYSQL_STMT *stmt, io::async_promise *prom, _borrow_para *para); // borrow a SQL thread (connect) to process multi-row fetch relative stuff. requires to entry the kernel mode
            void insert_base(MYSQL *my, MYSQL_STMT *stmt, async_promise_with_ptr *prom, MYSQL_BIND *bindr);
            void delete_base(MYSQL *my, MYSQL_STMT *stmt, io::async_promise *prom, const char *key, MYSQL_BIND *bindr);
            void update_base(MYSQL *my, MYSQL_STMT *stmt, io::async_promise *prom, const char *key, MYSQL_BIND *bindr);
            void select_base(MYSQL *my, MYSQL_STMT *stmt, async_promise_with_ptr *prom, MYSQL_BIND *bindr, const char *key);

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