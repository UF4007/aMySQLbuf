
template <typename TableStruct, typename... Indexs>
inline table<TableStruct, Indexs...>::table(eb::manager *mngr, std::vector<eb::base::mysql_meta> metadata, connect_conf_t connect_conf, std::vector<const char *> index_name, size_t map_size, size_t connect_num) : tableMap(map_size, &metadata, &index_name)
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
    oss << "SELECT * FROM " << connect_conf.tablename << " ORDER BY " << index_name[0] << " ASC LIMIT ";
    this->instruction_loadall = oss.str();
    oss = std::ostringstream();
    oss << "SELECT * FROM " << connect_conf.tablename << " WHERE " << index_name[0] << " > ? ORDER BY " << index_name[0] << " ASC LIMIT ";
    this->instruction_loadall2 = oss.str();

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
template <typename T_FSM>
inline void table<TableStruct, Indexs...>::loadAll(io::fsm<T_FSM> &fsm, io::future &prom)
{
    io::promise p = fsm.make_future(prom);
    if (queue_count.load() > queue_overload_limit)
    {
        p.reject(std::make_error_code(std::errc::too_many_files_open));
        return;
    }
    auto built_in_coro = [](table<TableStruct, Indexs...> *this_, io::fsm<T_FSM> &fsm, io::promise<> pr) -> io::fsm_func<void>
    {
        size_t where_primary = this_->getMapWhereByKey(this_->index_name[0]);
        MYSQL_STMT *borrow;
        std::atomic_flag *done;
        std::vector<MYSQL_BIND> bindr(this_->metadata.size());
        std::vector<size_t> bind_length(this_->metadata.size());

        // SELECT * FROM table WHERE uid > ? ORDER BY uid ASC LIMIT 1000;
        std::string instr;
        constexpr int fetch_len = 1000;
        bool restring_instr = false;
        _borrow_para _para;
        _para.bind_in = nullptr;
        instr = this_->instruction_loadall;
        instr += std::to_string(fetch_len);
        instr += ";";
        _para.borrow = &borrow;
        _para.done = &done;
        _para.instr = instr.c_str();
        _para.instr_len = instr.size();
        io::async_future futuLocal;
        while (1)
        {
            io::async_promise promLocal = fsm.make_future(futuLocal);

            std::vector<Delegate> *queue = nullptr;
            while (queue == nullptr)
                queue = this_->queue_instr.inbound_get();

            queue->emplace_back(&table::genBorrow_base, &promLocal, &_para);

            this_->queue_instr.inbound_unlock(queue);
            if (this_->queue_count.fetch_add(1) == 0)
                this_->queue_count.notify_one();

            co_await futuLocal;

            if (!futuLocal.getErr())
            {
                // borrow successfully, fetch rows that were stored locally. it costs no io blocking.
                int i = 0;
                size_t num_rows = mysql_stmt_num_rows(borrow);
                while (i < num_rows)
                {
                    eb::dumbPtr<TableStruct> insertee = new TableStruct(this_->getManager());
                    insertee->SQL_bind(this_->metadata, bindr.data(), bind_length.data());
                    mysql_stmt_bind_result(borrow, bindr.data());
                    int ret = mysql_stmt_fetch(borrow);
                    if (ret == MYSQL_NO_DATA)
                    {
                        assert(false);
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
                            assert(false);
                            break;
                        }
                    }
                    this_->tableMap.load(insertee, bindr.data());
                    i++;
                }
                done->test_and_set(std::memory_order_release);
                done->notify_one();
                if (i == fetch_len) // there is data rows remaining
                {
                    _para.bind_in = &bindr[where_primary];
                    if (restring_instr == false)
                    {
                        instr = this_->instruction_loadall2;
                        instr += std::to_string(fetch_len);
                        instr += ";";
                        _para.instr = instr.c_str();
                        _para.instr_len = instr.size();
                        restring_instr = true;
                    }
                }
                else // no data rows remaining
                {
                    pr.resolve();
                    co_return;
                }
            }
            else
            {
                pr.reject(std::make_error_code(std::errc::invalid_argument));
                co_return;
            }
        }
        co_return;
    };
    fsm.spawn_now(built_in_coro(this, fsm, std::move(p))).detach();
}
template <typename TableStruct, typename... Indexs>
template <typename T_FSM>
inline void table<TableStruct, Indexs...>::insert(io::fsm<T_FSM> &fsm, io::future &prom, eb::dumbPtr<TableStruct> &pointer)
{
    io::promise p = fsm.make_future(prom);
    if (queue_count.load() > queue_overload_limit)
    {
        p.reject(std::make_error_code(std::errc::too_many_files_open));
        return;
    }
    auto built_in_coro = [](table<TableStruct, Indexs...> *this_, io::fsm<T_FSM> &fsm, io::promise<> pr, eb::dumbPtr<TableStruct> &pointer) -> io::fsm_func<void>
    {
        io::async_future futuLocal;
        async_promise_with_ptr promLocal;
        promLocal.prom = fsm.make_future(futuLocal);
        promLocal.ptr = pointer;
        std::vector<MYSQL_BIND> bindr(this_->metadata.size());

        std::vector<Delegate> *queue = nullptr;
        while (queue == nullptr)
            queue = this_->queue_instr.inbound_get();

        // INSERT INTO test ( name , str , time , arr ) VALUES ( ? , ? , ? , ? )RETURNING *;
        queue->emplace_back(&table::insert_base, &promLocal, bindr.data());

        this_->queue_instr.inbound_unlock(queue);
        if (this_->queue_count.fetch_add(1) == 0)
            this_->queue_count.notify_one();

        co_await futuLocal;
        if (!futuLocal.getErr() && this_->tableMap.load(promLocal.ptr, bindr.data()).isEmpty())
        {
            pr.resolve();
        }
        else
        {
            pr.reject(std::make_error_code(std::errc::invalid_argument));
        }
    };
    fsm.spawn_now(built_in_coro(this, fsm, std::move(p), pointer)).detach();
}
template <typename TableStruct, typename... Indexs>
template <typename Index, typename T_FSM>
inline void table<TableStruct, Indexs...>::deletee(io::fsm<T_FSM> &fsm, io::future &prom, const char *key, const Index &index)
{
    io::promise p = fsm.make_future(prom);
    if (queue_count.load() > queue_overload_limit)
    {
        p.reject(std::make_error_code(std::errc::too_many_files_open));
        return;
    }
    auto built_in_coro = [](table<TableStruct, Indexs...> *this_, io::fsm<T_FSM> &fsm, io::promise<> pr, const char *key, const Index &index) -> io::fsm_func<void>
    {
        io::async_future futuLocal;
        async_promise_with_ptr promLocal;
        promLocal.prom = fsm.make_future(futuLocal);

        std::vector<MYSQL_BIND> bindr(this_->metadata.size());

        this_->unloadLocal(key, index);

        // MySQL delete, whether the row loaded or not.
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
        this_->IndexBind(*bindr.data(), first);

        std::vector<Delegate> *queue = nullptr;
        while (queue == nullptr)
            queue = this_->queue_instr.inbound_get();

        // DELETE FROM test WHERE uid = ?
        queue->emplace_back(&table::delete_base, &promLocal, key, &bindr[0]);

        this_->queue_instr.inbound_unlock(queue);
        if (this_->queue_count.fetch_add(1) == 0)
            this_->queue_count.notify_one();

        co_await futuLocal;
        if (!futuLocal.getErr())
        {
            pr.resolve();
        }
        else
        {
            pr.reject(std::make_error_code(std::errc::invalid_argument));
        }
        co_return;
    };
    fsm.spawn_now(built_in_coro(this, fsm, std::move(p), key, index)).detach();
}
template <typename TableStruct, typename... Indexs>
template <typename Index, typename T_FSM>
inline void table<TableStruct, Indexs...>::update(io::fsm<T_FSM> &fsm, io::future &prom, const Index &primary_index)
{
    io::promise p = fsm.make_future(prom);
    if (queue_count.load() > queue_overload_limit)
    {
        p.reject(std::make_error_code(std::errc::too_many_files_open));
        return;
    }
    constexpr size_t index_where = 0;
    auto found = tableMap.selectAll(index_where, primary_index);
    // if found more than 1 primary index or not found, abort
    if (std::distance(found.first, found.second) != 1)
    {
        p.reject(std::make_error_code(std::errc::invalid_argument));
        return;
    }
    auto built_in_coro = [found](table<TableStruct, Indexs...> *this_, io::fsm<T_FSM> &fsm, io::promise<> pr) mutable -> io::fsm_func<void>
    {
        io::async_future futuLocal;
        async_promise_with_ptr promLocal;
        promLocal.prom = fsm.make_future(futuLocal);

        std::vector<MYSQL_BIND> bindr(this_->metadata.size());
        found.first->second->SQL_bind(bindr.data());

        std::vector<Delegate> *queue = nullptr;
        while (queue == nullptr)
            queue = this_->queue_instr.inbound_get();

        // UPDATE test SET name = ?, str = ?, time = ? WHERE uid = ?
        queue->emplace_back(&table::update_base, &promLocal, this_->index_name[index_where], &bindr[0]);

        this_->queue_instr.inbound_unlock(queue);
        if (this_->queue_count.fetch_add(1) == 0)
            this_->queue_count.notify_one();

        co_await futuLocal;
        if (!futuLocal.getErr())
        {
            pr.resolve();
        }
        else
        {
            pr.reject(std::make_error_code(std::errc::invalid_argument));
        }
    };
    fsm.spawn_now(built_in_coro(this, fsm, std::move(p))).detach();
}
template <typename TableStruct, typename... Indexs>
template <typename Index, typename T_FSM>
inline void table<TableStruct, Indexs...>::select(io::fsm<T_FSM> &fsm, futureTS &prom, const char *key, const Index &index)
{
    io::promise<eb::dumbPtr<TableStruct>> p = fsm.make_future(prom, &prom.data);

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
            p.resolve();
            return;
        }
    }

    if (queue_count.load() > queue_overload_limit)
    {
        p.reject(std::make_error_code(std::errc::too_many_files_open));
        return;
    }

    auto built_in_coro = [](table<TableStruct, Indexs...> *this_, io::fsm<T_FSM> &fsm, io::promise<eb::dumbPtr<TableStruct>> pr, const char *key, const Index &index) -> io::fsm_func<void>
    {
        io::async_future futuLocal;
        async_promise_with_ptr promLocal;
        promLocal.prom = fsm.make_future(futuLocal);
        promLocal.ptr = new TableStruct(this_->getManager());

        std::vector<MYSQL_BIND> bindr(this_->metadata.size());

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
        this_->IndexBind(*bindr.data(), first);

        std::vector<Delegate> *queue = nullptr;
        while (queue == nullptr)
            queue = this_->queue_instr.inbound_get();

        // SELECT * FROM test WHERE uid LIKE ? ORDER BY uid DESC LIMIT 1;
        queue->emplace_back(&table::select_base, &promLocal, &bindr[0], key);

        this_->queue_instr.inbound_unlock(queue);
        if (this_->queue_count.fetch_add(1) == 0)
            this_->queue_count.notify_one();

        co_await futuLocal;
        if (!futuLocal.getErr())
        {
            if (pr.valid())
            {
                auto found = this_->tableMap.load(promLocal.ptr, bindr.data());
                if (found.isEmpty()) // primary index has not found in exist loaded map, return new ptr
                    *pr.data() = promLocal.ptr;
                else // primary index has been found exist, use found ptr and release new
                    *pr.data() = found;
                pr.resolve();
            }
            co_return;
        }
        pr.reject(std::make_error_code(std::errc::too_many_files_open));
        co_return;
    };
    fsm.spawn_now(built_in_coro(this, fsm, std::move(p), key, index)).detach();
}
template <typename TableStruct, typename... Indexs>
template <typename Index, typename T_FSM>
inline void table<TableStruct, Indexs...>::selectAll(io::fsm<T_FSM> &fsm, futureTSV &prom, const char *key, const Index &index)
{
    io::promise<std::vector<eb::dumbPtr<TableStruct>>> p = fsm.make_future(prom, &prom.data);

    if (queue_count.load() > queue_overload_limit)
    {
        p.reject(std::make_error_code(std::errc::too_many_files_open));
        return;
    }
    prom.data.clear();
    auto built_in_coro = [](table<TableStruct, Indexs...> *this_, io::manager *mngr, io::promise<std::vector<eb::dumbPtr<TableStruct>>> pr, const char *key, const Index &index) -> io::fsm_func<void>
    {
        size_t where_primary = this_->getMapWhereByKey(this_->index_name[0]);
        MYSQL_STMT *borrow;
        std::atomic_flag *done;
        std::vector<MYSQL_BIND> bindr(this_->metadata.size());
        std::vector<size_t> bind_length(this_->metadata.size());

        MYSQL_BIND bind_param[2];
        std::memset(bind_param, 0, sizeof(MYSQL_BIND));
        Index first;
        if constexpr (std::is_array_v<Index>)
        {
            std::memcpy(&first[0], &index[0], sizeof(index));
        }
        else
        {
            first = index;
        }
        this_->IndexBind(bind_param[0], first);

        // SELECT * FROM table WHERE name LIKE ? AND uid > ? ORDER BY uid ASC LIMIT 1000;
        std::string instr;
        constexpr int fetch_len = 1000;
        bool restring_instr = false;
        _borrow_para _para;
        instr = this_->instruction_select;
        instr += key;
        instr += " LIKE ? ";
        instr += "ORDER BY ";
        instr += this_->metadata[where_primary].key;
        instr += " ASC LIMIT ";
        instr += std::to_string(fetch_len);
        instr += ";";
        _para.bind_in = bind_param;
        _para.borrow = &borrow;
        _para.done = &done;
        _para.instr = instr.c_str();
        _para.instr_len = instr.size();
        while (1)
        {
            io::async_future futuLocal;
            io::async_promise promLocal;
            promLocal = mngr->make_future(futuLocal);

            std::vector<Delegate> *queue = nullptr;
            while (queue == nullptr)
                queue = this_->queue_instr.inbound_get();

            queue->emplace_back(&table::genBorrow_base, &promLocal, &_para);

            this_->queue_instr.inbound_unlock(queue);
            if (this_->queue_count.fetch_add(1) == 0)
                this_->queue_count.notify_one();

            co_await futuLocal;

            if (!futuLocal.getErr())
            {
                // borrow successfully, fetch rows that were stored locally. it costs no io blocking.
                int i = 0;
                size_t num_rows = mysql_stmt_num_rows(borrow);
                while (i < num_rows)
                {
                    eb::dumbPtr<TableStruct> insertee = new TableStruct(this_->getManager());
                    insertee->SQL_bind(this_->metadata, bindr.data(), bind_length.data());
                    mysql_stmt_bind_result(borrow, bindr.data());
                    int ret = mysql_stmt_fetch(borrow);
                    if (ret == MYSQL_NO_DATA)
                    {
                        assert(false);
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
                            assert(false);
                            break;
                        }
                    }
                    auto found = this_->tableMap.load(insertee, bindr.data());
                    if (found.isFilled())
                    {
                        pr.data()->emplace_back(found);
                    }
                    else
                    {
                        pr.data()->emplace_back(insertee);
                    }
                    i++;
                }
                done->test_and_set(std::memory_order_release);
                done->notify_one();
                if (i == fetch_len) // there is data rows remaining
                {
                    bind_param[1] = bindr[where_primary];
                    if (restring_instr == false)
                    {
                        instr = this_->instruction_select;
                        instr += key;
                        instr += " LIKE ? ";
                        instr += "AND ";
                        instr += this_->metadata[where_primary].key;
                        instr += " > ? ";
                        instr += "ORDER BY ";
                        instr += this_->metadata[where_primary].key;
                        instr += " ASC LIMIT ";
                        instr += std::to_string(fetch_len);
                        instr += ";";
                        _para.instr = instr.c_str();
                        _para.instr_len = instr.size();
                        restring_instr = true;
                    }
                }
                else // no data rows remaining
                {
                    pr.resolve();
                    co_return;
                }
            }
            else
            {
                pr.reject(std::make_error_code(std::errc::too_many_files_open));
                co_return;
            }
        }
        co_return;
    };
    fsm.spawn_now(built_in_coro(this, fsm.getManager(), std::move(p), key, index)).detach();
}



template <typename TableStruct, typename... Indexs>
template <typename Index, typename Index_alt>
inline void table<TableStruct, Indexs...>::relocateIndexLocal(eb::dumbPtr<TableStruct> &ptr, const char *key, const Index &old_value, const Index_alt &new_value)
{
    size_t index_where = getMapWhereByKey(key);
    return tableMap.relocateIndex(ptr, index_where, old_value, new_value);
}
template <typename TableStruct, typename... Indexs>
template <typename Index>
inline eb::dumbPtr<TableStruct> table<TableStruct, Indexs...>::selectLocal(const char *key, const Index &index)
{
    size_t index_where = getMapWhereByKey(key);
    return tableMap.select(index_where, index);
}
template <typename TableStruct, typename... Indexs>
template <typename Index>
inline auto table<TableStruct, Indexs...>::selectLocalAll(const char *key, const Index &index)
{
    size_t index_where = getMapWhereByKey(key);
    return tableMap.selectAll(index_where, index);
}
template <typename TableStruct, typename... Indexs>
template <typename Index>
inline void table<TableStruct, Indexs...>::unloadLocal(const char *key, const Index &index)
{
    size_t index_where = getMapWhereByKey(key);
    MYSQL_BIND bindr[metadata.size()];
    eb::dumbPtr<TableStruct> found, lastFound;

    // for erasing the records of this value in all index maps.
    while (1)
    {
        found = tableMap.select(index_where, index);
        if (found == lastFound) // prevent from dead loop
            break;
        lastFound = found;
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
inline void table<TableStruct, Indexs...>::clear()
{
    tableMap.clear();
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
                    std::invoke(deleg, this, my, stmt);
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
inline void table<TableStruct, Indexs...>::insert_base(MYSQL *my, MYSQL_STMT *stmt, async_promise_with_ptr *prom, MYSQL_BIND *bindr)
{
    eb::dumbPtr<TableStruct> &insertee = prom->ptr;
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
                prom->prom.reject(std::make_error_code(std::errc::too_many_files_open));
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
                    prom->prom.reject(std::make_error_code(std::errc::too_many_files_open));
                    break;
                }
            }
            prom->prom.resolve();
        } while (0);
        mysql_stmt_free_result(stmt);
    }
    else
    {
        prom->prom.reject(std::make_error_code(std::errc::too_many_files_open));
    }
}
template <typename TableStruct, typename... Indexs>
inline void table<TableStruct, Indexs...>::delete_base(MYSQL *my, MYSQL_STMT *stmt, io::async_promise *prom, const char *key, MYSQL_BIND *bindr)
{
    thread_local std::string instr(this->instruction_delete.c_str()); // copy anyway
    size_t instr_size = instr.size();
    instr += key;
    instr += " = ?;";

    mysql_stmt_prepare(stmt, instr.c_str(), instr.size());
    mysql_stmt_bind_param(stmt, bindr);
    if (mysql_stmt_execute(stmt) == 0)
        prom->resolve();
    else
        prom->reject(std::make_error_code(std::errc::too_many_files_open));

    instr.resize(instr_size);
}
template <typename TableStruct, typename... Indexs>
inline void table<TableStruct, Indexs...>::update_base(MYSQL *my, MYSQL_STMT *stmt, io::async_promise *prom, const char *key, MYSQL_BIND *bindr)
{
    thread_local std::string instr(this->instruction_update.c_str()); // copy anyway
    MYSQL_BIND bind[metadata.size()];
    size_t instr_size = instr.size();
    bool first = true;
    size_t sum = 0, i = 0;
    for (const auto &meta : metadata)
    {
        bool is_break = false;
        // exclude primary index
        if (std::strcmp(meta.key, key) == 0)
        {
            // WHERE = ?
            bind[metadata.size() - 1] = bindr[i];
            is_break = true;
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
        prom->resolve();
    else
        prom->reject(std::make_error_code(std::errc::too_many_files_open));

    instr.resize(instr_size);
}
template <typename TableStruct, typename... Indexs>
inline void table<TableStruct, Indexs...>::select_base(MYSQL *my, MYSQL_STMT *stmt, async_promise_with_ptr *prom, MYSQL_BIND *bindr, const char *key)
{
    thread_local std::string instr(this->instruction_select.c_str()); // copy anyway
    size_t instr_size = instr.size();
    instr += key;
    instr += " LIKE ? ORDER BY ";
    instr += index_name[0];
    instr += " DESC LIMIT 1;";

    eb::dumbPtr<TableStruct> &insertee = prom->ptr;
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
            prom->prom.reject(std::make_error_code(std::errc::too_many_files_open));
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
                prom->prom.reject(std::make_error_code(std::errc::too_many_files_open));
                break;
            }
        }
        prom->prom.resolve();
    } while(0);
    mysql_stmt_free_result(stmt);

    instr.resize(instr_size);
}
template <typename TableStruct, typename... Indexs>
inline void table<TableStruct, Indexs...>::genBorrow_base(MYSQL *my, MYSQL_STMT *stmt, io::async_promise *prom, _borrow_para *para)
{
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    *para->borrow = stmt;
    *para->done = &flag;
    mysql_stmt_prepare(stmt, para->instr, para->instr_len);
    if (para->bind_in)
        mysql_stmt_bind_param(stmt, para->bind_in);
    if (mysql_stmt_execute(stmt) == 0)
    {
        mysql_stmt_store_result(stmt);
        prom->resolve();
        flag.wait(0);
        mysql_stmt_free_result(stmt);
    }
    else
        prom->reject(std::make_error_code(std::errc::too_many_files_open));
}