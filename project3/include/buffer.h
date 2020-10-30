#ifndef BUFFER_H_
#define BUFFER_H_

#include "common.h"
#include "dbms.h"
#include "file.h"
#include "page.h"

#include <memory>
#include <optional>
#include <unordered_map>

class BufferBlock final
{
 public:
    void lock();
    void unlock();

    [[nodiscard]] page_t& frame();

    [[nodiscard]] constexpr table_id_t table_id() noexcept
    {
        return table_id_;
    }
    [[nodiscard]] constexpr pagenum_t pagenum() noexcept
    {
        return pagenum_;
    }

    void mark_dirty() noexcept;

 private:
    void clear();

 private:
    page_t* frame_;
    table_id_t table_id_{ -1 };
    pagenum_t pagenum_{ NULL_PAGE_NUM };

    bool is_dirty_{ false };
    int pin_count_{ 0 };

    BufferBlock* prev_{ nullptr };
    BufferBlock* next_{ nullptr };

    friend class BufferManager;
};

class BufferManager final
{
 public:
    [[nodiscard]] bool initialize(int num_buf);
    [[nodiscard]] bool shutdown();

    [[nodiscard]] bool close_table(int table_id);

    [[nodiscard]] bool get_page(table_id_t table_id, pagenum_t pagenum,
                                std::optional<Page>& page);

    bool check_all_unpinned() const;
    void dump_frame_stat() const;

 private:
    BufferManager() = default;

    void enqueue(BufferBlock* block);
    void unlink_and_enqueue(BufferBlock* block);

    [[nodiscard]] BufferBlock* eviction();
    [[nodiscard]] BufferBlock* eviction(BufferBlock* block);

 private:
    BufferBlock* head_{ nullptr };
    BufferBlock* tail_{ nullptr };

    page_t* page_arr_{ nullptr };

    std::unordered_map<table_page_t, BufferBlock*> block_tbl_;
    
    friend class DBMS;
};

template <typename Function>
[[nodiscard]] bool buffer(Function&& func, table_id_t table_id,
                          pagenum_t pagenum = NULL_PAGE_NUM)
{
    std::optional<Page> opt;
    CHECK_FAILURE(
        BufMgr().get_page(std::move(table_id), pagenum, opt));

    return func(opt.value());
}

#endif  // BUFFER_H_
