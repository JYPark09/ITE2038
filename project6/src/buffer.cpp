#include "buffer.h"

#include "log.h"
#include "page.h"

#include <memory.h>
#include <cassert>

#include <iostream>

void BufferBlock::lock(bool lock)
{
    ++pin_count_;

    // if (lock)
    //     mutex_.lock();
}

void BufferBlock::unlock()
{
    assert(pin_count() > 0);
    // mutex_.unlock();
    --pin_count_;
}

page_t& BufferBlock::frame()
{
    assert(pin_count() > 0);
    return *frame_;
}

void BufferBlock::mark_dirty() noexcept
{
    is_dirty_ = true;
}

void BufferBlock::clear()
{
    memset(frame_, 0, PAGE_SIZE);

    table_id_ = -1;
    pagenum_ = NULL_PAGE_NUM;

    is_dirty_ = false;
    pin_count_ = 0;
}

int BufferBlock::pin_count() const
{
    return pin_count_.load();
}

bool BufferManager::initialize(int num_buf)
{
    CHECK_FAILURE(instance_ == nullptr);

    instance_ = new (std::nothrow) BufferManager;
    CHECK_FAILURE(instance_ != nullptr);

    CHECK_FAILURE(instance_->init_lru(num_buf));

    return FileManager::initialize();
}

bool BufferManager::shutdown()
{
    CHECK_FAILURE(instance_ != nullptr);

    CHECK_FAILURE(instance_->shutdown_lru());

    CHECK_FAILURE(FileManager::shutdown());

    delete instance_;
    instance_ = nullptr;

    return true;
}

BufferManager& BufferManager::get_instance()
{
    return *instance_;
}

bool BufferManager::init_lru(int num_buf)
{
    if (num_buf <= 0)
    {
        return false;
    }

    page_arr_ = new (std::nothrow) page_t[num_buf];
    CHECK_FAILURE(page_arr_ != nullptr);

    for (int i = 0; i < num_buf; ++i)
    {
        BufferBlock* block = new (std::nothrow) BufferBlock;
        CHECK_FAILURE(block != nullptr);

        block->frame_ = &page_arr_[i];

        enqueue(block);
    }

    return true;
}

bool BufferManager::shutdown_lru()
{
    // list is empty
    if (head_ == nullptr)
    {
        return true;
    }

    BufferBlock* tmp;
    BufferBlock* current = head_;
    do
    {
        while (current->pin_count() > 0)
            ;

        CHECK_FAILURE(clear_block(current));

        tmp = current->next_;
        delete current;
        current = tmp;
    } while (current != head_);

    delete[] page_arr_;

    return true;
}

bool BufferManager::open_table(Table& table)
{
    return FileMgr().open_table(table);
}

bool BufferManager::close_table(Table& table)
{
    const table_id_t tid = table.id();

    for (const auto& pr : block_tbl_)
    {
        if (pr.second->table_id() != tid)
            continue;

        while (pr.second->pin_count() > 0)
            ;

        CHECK_FAILURE(clear_block(pr.second));
    }

    for (auto it = begin(block_tbl_); it != end(block_tbl_);)
    {
        if (it->second->table_id() == tid)
        {
            it = block_tbl_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    return FileMgr().close_table(table);
}

bool BufferManager::sync_all()
{
    std::scoped_lock lock(mutex_);

    for (auto& pr : block_tbl_)
    {
        CHECK_FAILURE(clear_block(pr.second));
    }

    return true;
}

bool BufferManager::create_page(Table& table, bool is_leaf, pagenum_t& pagenum)
{
    pagenum = NULL_PAGE_NUM;

    return buffer(
        [&](Page& header) {
            CHECK_FAILURE(table.file()->file_alloc_page(header, pagenum));

            return buffer(
                [&](Page& new_page) {
                    new_page.clear();

                    new_page.header().is_leaf = is_leaf;

                    new_page.mark_dirty();

                    return true;
                },
                table, pagenum);
        },
        table);
}

bool BufferManager::free_page(Table& table, pagenum_t pagenum)
{
    return buffer(
        [&](Page& header) {
            return buffer(
                [&](Page& free_page) {
                    free_page.free_header().next_free_page_number =
                        header.header_page().free_page_number;

                    header.header_page().free_page_number = pagenum;

                    free_page.mark_dirty();
                    header.mark_dirty();

                    return true;
                },
                table, pagenum);
        },
        table);
}

bool BufferManager::get_page(Table& table, pagenum_t pagenum,
                             std::optional<Page>& page, bool page_lock)
{
    std::scoped_lock lock(mutex_);

    table_id_t table_id = table.id();

    BufferBlock* current = head_;

    auto it = block_tbl_.find({ table_id, pagenum });

    if (it != end(block_tbl_))
    {
        current = it->second;
        current->lock(page_lock);
    }
    else
    {
        if (current->table_id_ != -1)
            current = eviction();

        CHECK_FAILURE(current != nullptr);

        current->lock(page_lock);

        current->table_id_ = table_id;
        current->pagenum_ = pagenum;

        CHECK_FAILURE(table.file()->file_read_page(pagenum, current->frame_));

        block_tbl_.insert_or_assign({ table_id, pagenum }, current);
    }

    unlink_and_enqueue(current);
    page.emplace(*current);

    return true;
}

void BufferManager::enqueue(BufferBlock* block)
{
    // <- past       new ->
    //  head <> ... <> tail

    assert((head_ == nullptr && tail_ == nullptr) ||
           (head_ != nullptr && tail_ != nullptr));

    // when linked list is empty
    if (head_ == nullptr)
    {
        head_ = block;
        tail_ = block;

        head_->next_ = block;
        tail_->prev_ = block;
    }
    else
    {
        block->prev_ = tail_;
        tail_->next_ = block;
        tail_ = block;
    }

    head_->prev_ = tail_;
    tail_->next_ = head_;
}

void BufferManager::unlink_and_enqueue(BufferBlock* block)
{
    BufferBlock* prev = block->prev_;
    BufferBlock* next = block->next_;

    block->prev_ = nullptr;
    block->next_ = nullptr;

    prev->next_ = next;
    next->prev_ = prev;

    if (block == head_)
    {
        head_ = next;
    }
    if (block == tail_)
    {
        tail_ = prev;
    }

    enqueue(block);
}

BufferBlock* BufferManager::eviction()
{
    BufferBlock* victim = head_;
    while (victim->pin_count() > 0)
    {
        victim = victim->next_;
        assert(victim != head_);
    }

    return eviction(victim);
}

BufferBlock* BufferManager::eviction(BufferBlock* block)
{
    // dump_frame_stat();

    const table_id_t table_id = block->table_id();
    const pagenum_t pagenum = block->pagenum();

    CHECK_FAILURE2(clear_block(block), nullptr);
    block_tbl_.erase({ table_id, pagenum });

    return block;
}

bool BufferManager::clear_block(BufferBlock* block)
{
    const table_id_t table_id = block->table_id();
    const pagenum_t pagenum = block->pagenum();

    if (block->is_dirty_)
    {
        if (LogMgr().find_page(table_id, pagenum))
            CHECK_FAILURE(LogMgr().force());

        CHECK_FAILURE(
            TblMgr().get_table(table_id).value()->file()->file_write_page(
                pagenum, block->frame_));
    }

    block->clear();

    return true;
}
