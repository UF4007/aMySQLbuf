//test version: Mariadb 10.11.6
#include "aMySQLbuf.h"

using namespace asql;

struct asqlTU: mem::memUnit{
    uint64_t uid;
    char name[10];
    std::string str;
    MYSQL_TIME time;

    std::vector<int> irrelevant;
    void save_fetch(mem::memPara para)override{
        GWPP_SQL_READ("uid", uid, para); // read only, AUTO_INCREMENT was set in SQL
        GWPP("name", name, para);
        GWPP("irrelevant", irrelevant, para); // unsupported variable type will have nothing to do with the MySQL table.
        GWPP("str", str, para);
        GWPP_SQL_TIME("time", time, para);
    }
    asqlTU(mem::memManager *m):memUnit(m){}
};

io::ioManager workingThread;

io::coTask workingCoro(io::coPara para)
{
    // set database config
    connect_conf_t sw_database;
    sw_database.host = "127.0.0.1";
    sw_database.user = "starwars";
    sw_database.passwd = "1";
    sw_database.db = "starwars";
    sw_database.tablename = "test";
    sw_database.port = 3306;

    // get metadata info of memUnit type
    auto metadata = mem::memUnit::get_SQL_metadata<asqlTU>();

    // create table
    table<asqlTU, uint64_t, std::string> testTable = table<asqlTU, uint64_t, std::string>(nullptr,        // not use memManager, cannot use normal serialize/deserialize, and mem garbage collector. if you need, add one.
                                                                                          metadata,       //
                                                                                          sw_database,    //
                                                                                          {"uid", "str"}, // Index column name
                                                                                          1000,           // initial size of hash bucket
                                                                                          1               // connect sum(one connect as one thread)
    );
    io::coPromise<mem::dumbPtr<asqlTU>> promPtr(para.mngr);
    io::coPromise<> prom(para.mngr);

    //load all
    prom.reset();
    testTable.loadAll(prom);
    task_await(prom);
    if (prom.isCompleted())
        std::cout << "load all success! total size: " << testTable.size() << std::endl;
    else
        std::cout << "load all failed!" << std::endl;

    while (1)
    {
        // select Artanis
        promPtr.reset();
        testTable.select(promPtr, "name", "Artanis%");
        task_await(promPtr);
        if (promPtr.isCompleted())
            std::cout << "select Artanis success!" << std::endl;
        else
            std::cout << "select Artanis failed!" << std::endl;



        // insert
        promPtr.reset();
        mem::memPtr<asqlTU> test1 = new asqlTU(nullptr);
        *promPtr.data() = test1;
        promPtr.data()->operator*()->str = "test 1";
        testTable.insert(promPtr);
        task_await(promPtr);

        promPtr.reset();
        mem::memPtr<asqlTU> test2 = new asqlTU(nullptr);
        *promPtr.data() = test2;
        // promPtr.data()->operator*()->str = "test 1";
        promPtr.data()->operator*()->str = "test 2";
        testTable.insert(promPtr);
        task_await(promPtr);
        if (promPtr.isCompleted())
            std::cout << "insert success!" << std::endl;
        else
            std::cout << "insert failed!" << std::endl;



        // update test 2 -> Artanis and time
        prom.reset();
        test2->time = mem::memUnit::tp_to_SQL_TIME(std::chrono::system_clock::now() + std::chrono::hours(8));
        std::strcpy(test2->name, "Artanis");
        testTable.update(prom, "uid", test2->uid);
        task_await(prom);
        if (prom.isCompleted())
            std::cout << "update success!" << std::endl;
        else
            std::cout << "update failed!" << std::endl;



        // delete all test 1, whether in the memory or SQL
        prom.reset();
        testTable.deletee(prom, "str", "test 1");
        task_await(prom);
        if (prom.isCompleted())
            std::cout << "delete success!" << std::endl;
        else
            std::cout << "delete failed!" << std::endl;
    }

    //workingThread.once(workingCoro);    //memory leak test.
}

void asql_testmain(){
    workingThread.once(workingCoro);
    while(1)
    {
        workingThread.drive();
    }
}