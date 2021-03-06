#ifndef DBAPI_H_
#define DBAPI_H_

#include <cstdint>

extern "C" {
int init_db(int num_buf);
int shutdown_db();

int open_table(char* pathname);
int close_table(int table_id);

int db_insert(int table_id, int64_t key, char* value);
int db_find(int table_id, int64_t key, char* ret_val);
int db_delete(int table_id, int64_t key);
}

#endif  // DBAPI_H_
