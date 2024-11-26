
template <typename TableStruct, typename... Indexs>
inline table<TableStruct, Indexs...>::table(mem::memManager *mngr, std::vector<mem::memUnit::mysql_meta> metadata, connect_conf_t connect_conf, std::vector<const char *> index_name, size_t map_size, size_t connect_num) : tableMap(map_size, &metadata, &index_name)
{
    this->index_name = index_name;
    this->metadata = metadata;
    this->memMngr = mngr;

    // ready for the each instructions
    std::ostringstream oss;

    // insert
    oss << "INSERT INTO " << connect_conf.tablename << " ( ";
    bool first = true;
    for (const auto &meta : metadata)
    {
        if (meta.readonly == false)
        {
            if (!first)
            {
                oss << " ,";
            }
            oss << meta.key;
            first = false;
            metadata_write_sum++;
        }
    }
    oss << " ) VALUES ( ";
    first = true;
    for (const auto &meta : metadata)
    {
        if (meta.readonly == false)
        {
            if (!first)
            {
                oss << " ,";
            }
            oss << "?";
            first = false;
        }
    }
    oss << " )RETURNING *;";
    this->instruction_insert = oss.str();

    //delete
    oss = std::ostringstream();
    oss << "DELETE FROM " << connect_conf.tablename << " WHERE ";
    this->instruction_delete = oss.str();

    //update
    oss = std::ostringstream();
    oss << "UPDATE " << connect_conf.tablename << " SET ";
    this->instruction_update = oss.str();

    //select
    oss = std::ostringstream();
    oss << "SELECT * FROM " << connect_conf.tablename << " WHERE ";
    this->instruction_select = oss.str();

    //loadall
    oss = std::ostringstream();
    oss << "SELECT * FROM " << connect_conf.tablename << ";";
    this->instruction_loadall = oss.str();

    //lock table read
    oss = std::ostringstream();
    oss << "LOCK TABLES " << connect_conf.tablename << " READ;";
    this->instruction_locktable_read = oss.str();

    //unlock table
    this->instruction_unlocktable = "UNLOCK TABLES;";

    for (int i = 0; i < connect_num; i++)
    {
        this->thread_s.emplace_back(std::thread(&table::thread_f, this, connect_conf));
    }
}



template <typename TableStruct, typename... Indexs>
inline io::coTask table<TableStruct, Indexs...>::loadAll(io::coPromise<> &prom)
{
    if (queue_count.load() > queue_overload_limit)
    {
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.abortLocal();
            co_return;
        }
    }
    MYSQL_STMT* borrow;
    std::atomic_flag *done;
    std::vector<MYSQL_BIND> bindr(metadata.size());
    std::vector<size_t> bind_length(metadata.size());

    io::coPromiseStack<> _promLocal(prom.getManager());
    io::coPromise<> promLocal = _promLocal;

    std::vector<Delegate> *queue = nullptr;
    while (queue == nullptr)
        queue = queue_instr.inbound_get();

    // SELECT * FROM table;
    queue->emplace_back(&table::loadall_base, promLocal, &borrow, &done);

    queue_instr.inbound_unlock(queue);
    if (queue_count.fetch_add(1) == 0)
        queue_count.notify_one();

    task_await(promLocal);

    if (promLocal.isCompleted())
    {
        // borrow successfully, fetch rows that were stored locally. it costs no io blocking.
        int i = 0;
        do
        {
            mem::dumbPtr<TableStruct> insertee = new TableStruct(this->getManager());
            insertee->SQL_bind(metadata, bindr.data(), bind_length.data());
            mysql_stmt_bind_result(borrow, bindr.data());
            int ret = mysql_stmt_fetch(borrow);
            if (ret == MYSQL_NO_DATA)
            {
                break;
            }
            if (ret == MYSQL_DATA_TRUNCATED)
            {
                insertee->SQL_checkstr(bindr.data());
                mysql_stmt_data_seek(borrow, i);
                mysql_stmt_bind_result(borrow, bindr.data());
                ret = mysql_stmt_fetch(borrow);
                if (ret == MYSQL_NO_DATA)
                {
                    break;
                }
                tableMap.load(insertee, bindr.data());
            }
            i++;
        } while (1);
        done->test_and_set(std::memory_order_release);
        done->notify_one();
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.completeLocal();
            co_return;
        }
    }
    if (prom.tryOccupy() == io::err::ok)
    {
        prom.abortLocal();
        co_return;
    }
    co_return;
}
template <typename TableStruct, typename... Indexs>
inline io::coTask table<TableStruct, Indexs...>::insert(promiseTS &prom)
{
    if (queue_count.load() > queue_overload_limit)
    {
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.abortLocal();
            co_return;
        }
    }

    io::coPromiseStack<mem::dumbPtr<TableStruct>> _promLocal(prom.getManager(), *prom.data());
    io::coPromise<mem::dumbPtr<TableStruct>> promLocal = _promLocal;
    std::vector<MYSQL_BIND> bindr(metadata.size());

    std::vector<Delegate> *queue = nullptr;
    while (queue == nullptr)
        queue = queue_instr.inbound_get();

    // INSERT INTO test ( name , str , time , arr ) VALUES ( ? , ? , ? , ? )RETURNING *;
    queue->emplace_back(&table::insert_base, promLocal, bindr.data());

    queue_instr.inbound_unlock(queue);
    if (queue_count.fetch_add(1) == 0)
        queue_count.notify_one();

    task_await(promLocal);
    if (promLocal.isCompleted() && tableMap.load(*promLocal.data(), bindr.data()).isEmpty())
    {
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.completeLocal();
            co_return;
        }
    }
    else
    {
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.abortLocal();
            co_return;
        }
    }
    co_return;
}
template <typename TableStruct, typename... Indexs>
template <typename Index>
inline io::coTask table<TableStruct, Indexs...>::deletee(io::coPromise<> &prom, const char *key, const Index &index)
{
    if (queue_count.load() > queue_overload_limit)
    {
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.abortLocal();
            co_return;
        }
    }

    io::coPromiseStack<> _promLocal(prom.getManager());
    io::coPromise<> promLocal = _promLocal;
    std::vector<MYSQL_BIND> bindr(metadata.size());

    this->unloadMap(key, index);

    // MySQL delete, whether the row loaded or not.
    Index first;
    if constexpr(std::is_array_v<Index>)
    {
        std::memcpy(&first[0], &index[0], sizeof(index));
    }
    else
    {
        first = index;
    }
    std::memset(bindr.data(), 0 ,sizeof(MYSQL_BIND));
    IndexBind(*bindr.data(), first);

    std::vector<Delegate> *queue = nullptr;
    while (queue == nullptr)
        queue = queue_instr.inbound_get();

    // DELETE FROM test WHERE uid = ?
    queue->emplace_back(&table::delete_base, promLocal, key, &bindr[0]);

    queue_instr.inbound_unlock(queue);
    if (queue_count.fetch_add(1) == 0)
        queue_count.notify_one();

    task_await(promLocal);
    if (promLocal.isCompleted())
    {
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.completeLocal();
            co_return;
        }
    }
    if (prom.tryOccupy() == io::err::ok)
    {
        prom.abortLocal();
        co_return;
    }
    co_return;
}
template <typename TableStruct, typename... Indexs>
template <typename Index>
inline io::coTask table<TableStruct, Indexs...>::update(io::coPromise<> &prom, const char *key, const Index &index)
{
    size_t index_where = getMapWhereByKey(key);
    auto found = tableMap.selectAll(index_where, index);

    //if found more than 1 or not found, abort
    if (queue_count.load() > queue_overload_limit || std::distance(found.first, found.second) != 1)
    {
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.abortLocal();
            co_return;
        }
    }

    io::coPromiseStack<> _promLocal(prom.getManager());
    io::coPromise<> promLocal = _promLocal;

    std::vector<MYSQL_BIND> bindr(metadata.size());
    found.first->second->SQL_bind(bindr.data());

    std::vector<Delegate> *queue = nullptr;
    while (queue == nullptr)
        queue = queue_instr.inbound_get();

    // UPDATE test SET name = ?, time = ?, arr = ? WHERE uid = ?
    queue->emplace_back(&table::update_base, promLocal, key, &bindr[0]);

    queue_instr.inbound_unlock(queue);
    if (queue_count.fetch_add(1) == 0)
        queue_count.notify_one();

    task_await(promLocal);
    if (promLocal.isCompleted())
    {
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.completeLocal();
            co_return;
        }
    }
    if (prom.tryOccupy() == io::err::ok)
    {
        prom.abortLocal();
        co_return;
    }
    co_return;
}
template <typename TableStruct, typename... Indexs>
template <typename Index>
inline io::coTask table<TableStruct, Indexs...>::select(promiseTS &prom, const char *key, const Index &index)
{
    bool is_map_index = true;
    for (auto &i : this->index_name)
    {
        if (std::strcmp(i, key) == 0)
        {
            is_map_index = false;
            break;
        }
    }

    if (is_map_index == false) // parameter index as an index in table<>
    {
        size_t index_where = getMapWhereByKey(key);
        auto found = tableMap.select(index_where, index);
        if (found.isFilled())
        {
            if (prom.tryOccupy() == io::err::ok)
            {
                *prom.data() = found;
                prom.completeLocal();
                co_return;
            }
        }
    }

    if (queue_count.load() > queue_overload_limit)
    {
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.abortLocal();
            co_return;
        }
    }
    else
    {
        io::coPromiseStack<mem::dumbPtr<TableStruct>> _promLocal(prom.getManager(), *prom.data());
        io::coPromise<mem::dumbPtr<TableStruct>> promLocal = _promLocal;
        *promLocal.data() = new TableStruct(this->getManager());
        std::vector<MYSQL_BIND> bindr(metadata.size());

        Index first;
        if constexpr (std::is_array_v<Index>)
        {
            std::memcpy(&first[0], &index[0], sizeof(index));
        }
        else
        {
            first = index;
        }
        std::memset(bindr.data(), 0, sizeof(MYSQL_BIND));
        IndexBind(*bindr.data(), first);

        std::vector<Delegate> *queue = nullptr;
        while (queue == nullptr)
            queue = queue_instr.inbound_get();

        // SELECT * FROM test WHERE uid = ?
        queue->emplace_back(&table::select_base, promLocal, &bindr[0], key);

        queue_instr.inbound_unlock(queue);
        if (queue_count.fetch_add(1) == 0)
            queue_count.notify_one();

        task_await(promLocal);
        if (promLocal.isCompleted())
        {
            if (prom.tryOccupy() == io::err::ok)
            {
                auto found = tableMap.load(*promLocal.data(), bindr.data());
                if (found.isEmpty()) // primary index has not found in exist loaded map, return new ptr
                    *prom.data() = *promLocal.data();
                else                // primary index has been found exist, use found ptr and release new
                    *prom.data() = found;
                prom.completeLocal();
                co_return;
            }
        }
        if (prom.tryOccupy() == io::err::ok)
        {
            prom.abortLocal();
            co_return;
        }
    }
}



template <typename TableStruct, typename... Indexs>
template <typename Index>
inline mem::dumbPtr<TableStruct> table<TableStruct, Indexs...>::selectMap(const char *key, const Index &index)
{
    size_t index_where = getMapWhereByKey(key);
    return tableMap.select(index_where, index);
}
template <typename TableStruct, typename... Indexs>
template <typename Index>
inline std::pair<
    typename std::unordered_multimap<Index, mem::dumbPtr<TableStruct>>::iterator,
    typename std::unordered_multimap<Index, mem::dumbPtr<TableStruct>>::iterator>
table<TableStruct, Indexs...>::selectMapAll(const char *key, const Index &index)
{
    size_t index_where = getMapWhereByKey(key);
    return tableMap.selectAll(index_where, index);
}
template <typename TableStruct, typename... Indexs>
template <typename Index>
inline void table<TableStruct, Indexs...>::unloadMap(const char *key, const Index &index)
{
    size_t index_where = getMapWhereByKey(key);
    MYSQL_BIND bindr[metadata.size()];
    mem::dumbPtr<TableStruct> found;

    // for erasing the records of this value in all index maps.
    while (1)
    {
        found = tableMap.select(index_where, index);
        if (found == nullptr)
            break;
        if (found.isEmpty())
            continue;
        found->SQL_bind(bindr);
        tableMap.unload(found, bindr);
        //found.release();
    }

    // for erasing the released values (empty memPtr).
    tableMap.erase(index_where, index);
}




template <typename TableStruct, typename... Indexs>
inline table<TableStruct, Indexs...>::~table()
{
    thread_stop.test_and_set();
    queue_count.store(100000);
    queue_count.notify_all();
    // wait for all mysql working threads shutdown, and remove table thread
    for (auto &th : this->thread_s)
    {
        th.join();
    }
}
template <typename TableStruct, typename... Indexs>
inline size_t table<TableStruct, Indexs...>::getMapWhereByKey(const char *key)
{
    int k = 0;
    for (auto &i : this->metadata)
    {
        if(std::strcmp(i.key, key) == 0)
        {
            return k;
        }
        k++;
    }
    assert(!"key name ERROR: cannot found Index key name!");
    return SIZE_MAX;
}
template <typename TableStruct, typename... Indexs>
template <typename Index>
inline void table<TableStruct, Indexs...>::IndexBind(MYSQL_BIND& bind, Index &index)
{
    if constexpr (std::is_same_v<Index, std::string>)
    {
        bind.buffer_type = MYSQL_TYPE_MEDIUM_BLOB;
        bind.buffer_length = index.size();
        bind.buffer = index.c_str();
    }
    else if constexpr (std::is_same_v<typename std::remove_extent<Index>::type, char>) //char[]
    {
        bind.buffer_type = MYSQL_TYPE_STRING;
        bind.buffer_length = std::strlen(index);    // '\0' will cause the stmt working wrong without any error!
        bind.buffer = index;
    }
    else if constexpr (std::is_arithmetic<Index>::value)
    {
        constexpr size_t size = sizeof(index);
        if constexpr (size == 1)
        {
            bind.buffer_type = MYSQL_TYPE_TINY;
        }
        else if constexpr (size == 2)
        {
            bind.buffer_type = MYSQL_TYPE_SHORT;
        }
        else if constexpr (size == 4)
        {
            bind.buffer_type = MYSQL_TYPE_LONG;
        }
        else if constexpr (size == 8)
        {
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
        }
        if constexpr (std::is_signed_v<Index>)
        {
            bind.is_unsigned = false;
        }
        else
        {
            bind.is_unsigned = true;
        }
        bind.buffer_length = sizeof(index);
        bind.buffer = &index;
    }
    else if constexpr(std::is_same_v<Index, MYSQL_TIME>)
    {
        bind.buffer_type = MYSQL_TYPE_DATETIME;
        bind.buffer_length = sizeof(index);
        bind.buffer = &index;
    }
    else
        assert(!"Index type is too odd.");
}



template <typename TableStruct, typename... Indexs>
inline void table<TableStruct, Indexs...>::thread_f(connect_conf_t connect_conf)
{
    MYSQL *my = nullptr;
    MYSQL_STMT *stmt = nullptr;
    my = mysql_init(NULL);
    if (my == nullptr)
        assert(!"database init error!");
    if (mysql_real_connect(my, connect_conf.host, connect_conf.user, connect_conf.passwd, connect_conf.db, connect_conf.port, NULL, 0) == nullptr)
        assert(!"database connect error!");
    stmt = mysql_stmt_init(my);
    if (stmt == nullptr)
        assert(!"stmt init error!");
    while (this->thread_stop.test(std::memory_order_acquire) == false)
    {
        if (this->queue_count.load() > 0)
        {
            std::vector<Delegate> *queue = nullptr;
            queue = this->queue_instr.outbound_get();
            if (queue)
            {
                size_t size = queue->size();
                if (size != 0)
                {
                    auto iter = queue->end() - 1;
                    Delegate deleg = std::move(*iter);
                    queue->erase(iter);
                    this->queue_instr.outbound_unlock(queue);
                    this->queue_count--;
                    deleg(this, my, stmt);
                }
                else
                {
                    this->queue_instr.outbound_unlock(queue);
                    this->queue_instr.outbound_rotate();
                }
            }
            else
                this->queue_instr.outbound_rotate();
        }
        else
        {
            queue_count.wait(0, std::memory_order_acquire);
        }
    }
    mysql_stmt_close(stmt);
    mysql_close(my);
}
template <typename TableStruct, typename... Indexs>
inline void table<TableStruct, Indexs...>::insert_base(MYSQL *my, MYSQL_STMT *stmt, promiseTS &prom, MYSQL_BIND* bindr)
{
    mem::dumbPtr<TableStruct> &insertee = *prom.data();
    if (insertee.isFilled())
    {
        size_t bind_length[metadata.size()];
        MYSQL_BIND bindw[metadata_write_sum];
        memset(bindw, 0, metadata_write_sum * sizeof(MYSQL_BIND));
        insertee->SQL_bind(metadata, bindr, bindw, bind_length);
        mysql_stmt_prepare(stmt, instruction_insert.c_str(), instruction_insert.size());
        mysql_stmt_bind_param(stmt, bindw);
        mysql_stmt_execute(stmt);
        mysql_stmt_bind_result(stmt, bindr);
        mysql_stmt_store_result(stmt);
        int ret = mysql_stmt_fetch(stmt);
        do
        {
            if (ret == MYSQL_NO_DATA)
            {
                prom.abort();
                break;
            }
            if (ret == MYSQL_DATA_TRUNCATED)
            {
                insertee->SQL_checkstr(bindr);
                mysql_stmt_data_seek(stmt, 0);
                mysql_stmt_bind_result(stmt, bindr);
                ret = mysql_stmt_fetch(stmt);
                if (ret == MYSQL_NO_DATA)
                {
                    prom.abort();
                    break;
                }
            }
            prom.complete();
        } while (0);
        mysql_stmt_free_result(stmt);
    }
    else
    {
        prom.abort();
    }
}
template <typename TableStruct, typename... Indexs>
inline void table<TableStruct, Indexs...>::delete_base(MYSQL *my, MYSQL_STMT *stmt, io::coPromise<> &prom, const char *key, MYSQL_BIND *bindr)
{
    thread_local std::string instr(this->instruction_delete.c_str()); // copy anyway
    size_t instr_size = instr.size();
    instr += key;
    instr += " = ?;";

    mysql_stmt_prepare(stmt, instr.c_str(), instr.size());
    mysql_stmt_bind_param(stmt, bindr);
    if (mysql_stmt_execute(stmt) == 0)
        prom.complete();
    else
        prom.abort();

    instr.resize(instr_size);
}
template <typename TableStruct, typename... Indexs>
inline void table<TableStruct, Indexs...>::update_base(MYSQL *my, MYSQL_STMT *stmt, io::coPromise<> &prom, const char *key, MYSQL_BIND *bindr)
{
    thread_local std::string instr(this->instruction_update.c_str()); // copy anyway
    MYSQL_BIND bind[metadata.size()];
    size_t instr_size = instr.size();
    bool first = true;
    size_t sum = 0, i = 0;
    for (const auto &meta : metadata)
    {
        bool is_break = false;
        //exclude index
        for (const auto& ik : index_name)
        {
            if (std::strcmp(meta.key, ik) == 0)
            {
                is_break = true;
                break;
            }
        }
        //WHERE = ?
        if (std::strcmp(meta.key, key) == 0)
        {
            bind[metadata.size() - 1] = bindr[i];
        }
        //exclude readonly
        if (meta.readonly == false && is_break == false)
        {
            if (!first)
            {
                instr += " ,";
            }
            bind[sum] = bindr[i];
            instr += meta.key;
            instr += " = ?";
            first = false;
            sum++;
        }
        i++;
    }
    if (sum != metadata.size() - 1)
        bind[sum] = bind[metadata.size() - 1];
    instr += " WHERE ";
    instr += key;
    instr += " = ?;";

    mysql_stmt_prepare(stmt, instr.c_str(), instr.size());
    mysql_stmt_bind_param(stmt, bind);
    if (mysql_stmt_execute(stmt) == 0)
        prom.complete();
    else
        prom.abort();

    instr.resize(instr_size);
}
template <typename TableStruct, typename... Indexs>
inline void table<TableStruct, Indexs...>::select_base(MYSQL *my, MYSQL_STMT *stmt, promiseTS &prom, MYSQL_BIND *bindr, const char *key)
{
    thread_local std::string instr(this->instruction_select.c_str()); // copy anyway
    size_t instr_size = instr.size();
    instr += key;
    instr += " LIKE ? ORDER BY ";
    instr += index_name[0];
    instr += " DESC LIMIT 1;";

    mem::dumbPtr<TableStruct> &insertee = *prom.data();
    size_t bind_length[metadata.size()];
    MYSQL_BIND bindw = *bindr;
    insertee->SQL_bind(metadata, bindr, bind_length);
    mysql_stmt_prepare(stmt, instr.c_str(), instr.size());
    mysql_stmt_bind_param(stmt, &bindw);
    mysql_stmt_execute(stmt);
    mysql_stmt_bind_result(stmt, bindr);
    mysql_stmt_store_result(stmt);
    int ret = mysql_stmt_fetch(stmt);
    do{
        if (ret == MYSQL_NO_DATA)
        {
            prom.abort();
            break;
        }
        if (ret == MYSQL_DATA_TRUNCATED)
        {
            insertee->SQL_checkstr(bindr);
            mysql_stmt_data_seek(stmt, 0);
            mysql_stmt_bind_result(stmt, bindr);
            ret = mysql_stmt_fetch(stmt);
            if (ret == MYSQL_NO_DATA)
            {
                prom.abort();
                break;
            }
        }
        prom.complete();
    } while(0);
    mysql_stmt_free_result(stmt);

    instr.resize(instr_size);
}
template <typename TableStruct, typename... Indexs>
inline void table<TableStruct, Indexs...>::loadall_base(MYSQL *my, MYSQL_STMT *stmt, io::coPromise<> &prom, MYSQL_STMT **borrow, std::atomic_flag **done)
{
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    *borrow = stmt;
    *done = &flag;
    mysql_stmt_prepare(stmt, instruction_loadall.c_str(), instruction_loadall.size());
    if (mysql_stmt_execute(stmt) == 0)
    {
        mysql_stmt_store_result(stmt);
        prom.complete();
        flag.wait(0);
        mysql_stmt_free_result(stmt);
    }
    else
        prom.abort();
}