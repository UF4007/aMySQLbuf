<div>
	<a style="text-decoration: none;" href="">
		<img src="https://img.shields.io/badge/C++-%2300599C.svg?logo=c%2B%2B&logoColor=white" alt="cpp">
	</a>
	<a style="text-decoration: none;" href="">
		<img src="https://ci.appveyor.com/api/projects/status/1acb366xfyg3qybk/branch/develop?svg=true" alt="building">
	</a>
	<a href="https://github.com/UF4007/memManager/blob/main/License.txt">
		<img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT">
	<a href="https://www.debian.org/">
		<img src="https://img.shields.io/badge/Linux-FCC624?style=for-the-badge&logo=linux&logoColor=black" alt="linux">
</div>

# aMySQLbuf
async C++20 coroutine-based high-performance MySQL buffer library

Our advanced aMySQLbuf is totally beyond the old Redis.

### dependency:
- ioManager

- memManager

### control flow:
```C++
SQLThread                  	asql::table<struct, ...index>				MainThread(coroutine)
InstructionQueue			
...		<---query---	relative params (keep lifetime) <---detemplate--	table member method, get coPormise
...				|							task_await(coPromise)
...				|
async execute			|
async return	---result-->	coroutine wake			--------------->	get result
```
### data structure:
each 'index' template in the asql::table struct has a built-in hashmap.

### bind and corresponding C++ type name:
- TINYINT ---------------------------------> int8  
- SMALLINT --------------------------------> int16  
- INT -------------------------------------> int32  
- BIGINT ----------------------------------> int64  
- FLOAT -----------------------------------> float  
- DOUBLE ----------------------------------> double  
- BINARY ----------------------------------> char[]  
- MEDIUMBLOB ------------------------------> std::string series  
- DATETIME --------------------------------> MYSQL_TIME, via GWPP_SQL_TIME()  

---EXPERIMENTAL LIBRARY---
