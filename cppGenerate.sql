-- --------------------------------------------------------
-- 主机:                           127.0.0.1
-- 服务器版本:                        10.11.6-MariaDB-0+deb12u1 - Debian 12
-- 服务器操作系统:                      debian-linux-gnu
-- HeidiSQL 版本:                  12.8.0.6908
-- --------------------------------------------------------

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET NAMES utf8 */;
/*!50503 SET NAMES utf8mb4 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;

-- 导出  存储过程 IoT.GenerateCppCode 结构
DELIMITER //
CREATE PROCEDURE `GenerateCppCode`(
	IN `table_name_input` VARCHAR(64)
)
BEGIN
    -- 声明变量
    DECLARE done INT DEFAULT FALSE;
    DECLARE col_name VARCHAR(64);
    DECLARE col_type VARCHAR(64);
    DECLARE arr_prefix VARCHAR(64);
    DECLARE typename VARCHAR(64);
    DECLARE col_max_length INT;
    DECLARE cpp_gwpp TEXT DEFAULT '';
    
    -- 创建游标，确保 SELECT 列数和 FETCH 变量数相同
    DECLARE cur CURSOR FOR 
        SELECT COLUMN_NAME, DATA_TYPE, IFNULL(CHARACTER_MAXIMUM_LENGTH, 0)
        FROM INFORMATION_SCHEMA.COLUMNS 
        WHERE TABLE_NAME = table_name_input 
        AND TABLE_SCHEMA = DATABASE();
    
    -- 声明继续处理程序
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = TRUE;
    
    SET typename = CONCAT(table_name_input, '_t');
    
    SET @cpp_code = CONCAT(
        'struct ', typename, ' : eb::base\n',
        '{\n'
    );
    
    OPEN cur;
    
    read_loop: LOOP
        FETCH cur INTO col_name, col_type, col_max_length;
        IF done THEN
            LEAVE read_loop;
        END IF;
        
        -- 转换MySQL类型到C++类型
        SET @cpp_type = CASE LOWER(col_type)
            WHEN 'tinyint' THEN 'int8_t'
            WHEN 'smallint' THEN 'int16_t'
            WHEN 'int' THEN 'int32_t'
            WHEN 'bigint' THEN 'int64_t'
            WHEN 'float' THEN 'float'
            WHEN 'double' THEN 'double'
            WHEN 'binary' THEN 'char'
            WHEN 'mediumblob' THEN 'std::string'
            WHEN 'datetime' THEN 'MYSQL_TIME'
            ELSE 'std::string'
        END;
        
        -- 添加成员变量声明
		  SET arr_prefix = CASE LOWER(col_type)
		   	WHEN 'binary' THEN CONCAT('[', col_max_length, '];\n')
            ELSE ';\n'
			END;
		
		  SET @cpp_code = CONCAT(@cpp_code, '    ', @cpp_type, ' ', col_name, arr_prefix);
        
        -- 准备GWPP语句
        IF col_type = 'datetime' THEN
            SET @gwpp_stmt = CONCAT('        GWPP_SQL_TIME("', col_name, '", ', col_name, ', para);\n');
        ELSE
            SET @gwpp_stmt = CONCAT('        GWPP("', col_name, '", ', col_name, ', para);\n');
        END IF;
        
        SET cpp_gwpp = CONCAT(cpp_gwpp, @gwpp_stmt);
    END LOOP;
    
    CLOSE cur;
    
    -- 添加save_fetch和构造函数
    SET @cpp_code = CONCAT(@cpp_code, 
        '\n    void save_fetch(eb::para para) override\n    {\n',
        cpp_gwpp,
        '    }\n\n',
        '    ', typename, '(eb::manager *m) : eb::base(m) {}\n',
        '};\n',
        '    using ',table_name_input,'_table_t = asql::table<',typename,', >;\n',
        '    inline ',table_name_input,'_table_t& getTable',table_name_input,'()\n',
        '        {
            static asql::connect_conf_t db = {
                .host = "127.0.0.1",
                .user = ???,
                .passwd = ???,
                .db = "',DATABASE(),'",
                .tablename = "',table_name_input,'",
                .port = 3306,
            };
            static auto metadata = eb::base::get_SQL_metadata<',typename,'>();
            static ',table_name_input,'_table_t _table = ',table_name_input,'_table_t(nullptr,  // not use memManager, cannot use normal serialize/deserialize, and mem garbage collector. if you need, add one.
                                            metadata, //
                                            db,       //
                                            {???}, // Index column name
                                            100,      // initial size of hash bucket
                                            1         // connect sum(one connect as one thread)
            ); 
            return _table;
        }\n'
    );
    
    SELECT @cpp_code AS Generated_CPP_Code;
END//
DELIMITER ;

/*!40103 SET TIME_ZONE=IFNULL(@OLD_TIME_ZONE, 'system') */;
/*!40101 SET SQL_MODE=IFNULL(@OLD_SQL_MODE, '') */;
/*!40014 SET FOREIGN_KEY_CHECKS=IFNULL(@OLD_FOREIGN_KEY_CHECKS, 1) */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40111 SET SQL_NOTES=IFNULL(@OLD_SQL_NOTES, 1) */;
