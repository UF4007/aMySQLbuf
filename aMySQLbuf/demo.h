//test version: Mariadb 10.11.6
#include "aMySQLbuf.h"

using namespace asql;

struct asqlTU: eb::base{
    uint64_t uid;
    char name[10];
    std::string str;
    MYSQL_TIME time;

    std::vector<int> irrelevant;
    void save_fetch(eb::para para)override{
        GWPP_SQL_READ("uid", uid, para); // read only, AUTO_INCREMENT was set in SQL
        GWPP("name", name, para);
        GWPP("irrelevant", irrelevant, para); // unsupported variable type will have nothing to do with the MySQL table.
        GWPP("str", str, para);
        GWPP_SQL_TIME("time", time, para);
    }
    asqlTU(eb::manager *m):base(m){}
};

io::manager workingThread;

io::fsm_func<void> workingCoro()
{
    io::fsm<void>& fsm = co_await io::get_fsm;
    // set database config
    connect_conf_t sw_database;
    sw_database.host = "127.0.0.1";
    sw_database.user = "starwars";
    sw_database.passwd = "1";
    sw_database.db = "starwars";
    sw_database.tablename = "test";
    sw_database.port = 3306;

    // get metadata info of eb::base type
    auto metadata = eb::base::get_SQL_metadata<asqlTU>();

    // create table
    table<asqlTU, uint64_t, std::string> testTable = table<asqlTU, uint64_t, std::string>(nullptr,        // not use memManager, cannot use normal serialize/deserialize, and mem garbage collector. if you need, add one.
                                                                                          metadata,       //
                                                                                          sw_database,    //
                                                                                          {"uid", "str"}, // Index column name
                                                                                          1000,           // initial size of hash bucket
                                                                                          1               // connect sum(one connect as one thread)
    );
    io::future future;
    io::future_with<eb::dumbPtr<asqlTU>> future_ptr;
    io::future_with<std::vector<eb::dumbPtr<asqlTU>>> future_vec;

    //load all | select all
    if (false)
    {
        testTable.loadAll(fsm, future);
        co_await future;
        if (!future.getErr())
            std::cout << "load all success! total size: " << testTable.size() << std::endl;
        else
            std::cout << "load all failed!" << std::endl;
    }
    else
    {
        testTable.selectAll(fsm, future_vec, "str", "test 3%");
        co_await future_vec;
        if (!future_vec.getErr())
            std::cout << "load all success! total size: " << testTable.size() << std::endl;
        else
            std::cout << "load all failed!" << std::endl;
    }

    while (1)
    {
        // select Artanis
        testTable.select(fsm, future_ptr, "name", "Artanis%");
        co_await future_ptr;
        if (!future_ptr.getErr())
            std::cout << "select Artanis success!" << std::endl;
        else
            std::cout << "select Artanis failed!" << std::endl;



        // insert
        eb::memPtr<asqlTU> test1 = new asqlTU(nullptr);
        test1->str = "test 1";
        testTable.insert(fsm, future, test1);
        co_await future;

        eb::memPtr<asqlTU> test2 = new asqlTU(nullptr);
        // test2->str = "test 1";
        test2->str = "test 2";
        testTable.insert(fsm, future, test2);
        co_await future;
        if (!future.getErr())
            std::cout << "insert success!" << std::endl;
        else
            std::cout << "insert failed!" << std::endl;



        // relocate test 2 -> test 3
        testTable.relocateIndexLocal(test2, "str", test2->str, "test 3");
        test2->str = "test 3";



        // update test 2 -> Artanis and time
        test2->time = eb::base::tp_to_SQL_TIME(std::chrono::system_clock::now() + std::chrono::hours(8));
        std::strcpy(test2->name, "Artanis");
        testTable.update(fsm, future, test2->uid);
        co_await future;
        if (!future.getErr())
            std::cout << "update success!" << std::endl;
        else
            std::cout << "update failed!" << std::endl;



        // delete all test 1, whether in the memory or SQL
        testTable.deletee(fsm, future, "str", "test 1");
        co_await future;
        if (!future.getErr())
            std::cout << "delete success!" << std::endl;
        else
            std::cout << "delete failed!" << std::endl;
    }

    //workingThread.once(workingCoro);    //memory leak test.
}

void asql_testmain(){
    workingThread.spawn_later(workingCoro()).detach();
    while(1)
    {
        workingThread.drive();
    }
}