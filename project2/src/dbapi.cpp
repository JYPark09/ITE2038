#include "dbapi.h"

#include "bpt.h"
#include "common.h"

#include <cstring>

#include <iostream>

int open_table(char* pathname)
{
    static int TABLE_COUNT = 0;

    if (!BPTree::get().open(pathname))
        return -1;

    return TABLE_COUNT++;
}

int db_insert(int64_t key, char* value)
{
    if (!BPTree::get().is_open())
        return FAIL;

    page_data_t record;
    record.key = key;
    strncpy(record.value, value, PAGE_DATA_VALUE_SIZE);

    return BPTree::get().insert(record) ? SUCCESS : FAIL;
}

int db_find(int64_t key, char* ret_val)
{
    if (!BPTree::get().is_open())
        return FAIL;

    auto res = BPTree::get().find(key);
    if (!res)
        return FAIL;

    strncpy(ret_val, res.value().value, PAGE_DATA_VALUE_SIZE);
    return SUCCESS;
}

int db_delete(int64_t key)
{
    if (!BPTree::get().is_open())
        return FAIL;

    return BPTree::get().remove(key) ? SUCCESS : FAIL;
}
