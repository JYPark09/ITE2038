#include "recovery.h"

#include "buffer.h"
#include "table.h"

#include <fcntl.h>
#include <unistd.h>
#include <cassert>

#include <iostream>

Recovery::Recovery(const std::string& logmsg_path, RecoveryMode mode,
                   int log_num)
    : mode_(mode), log_num_(log_num)
{
    f_log_msg_.open(logmsg_path);
    assert(f_log_msg_.is_open());
}

Recovery::~Recovery()
{
    f_log_msg_.close();
}

void Recovery::start()
{
    analyse();

    if (!redo())
    {
        assert(TblMgr().close_all_tables());
        return;
    }

    // if (!undo())
    // {
    //     assert(TblMgr().close_all_tables());
    //     return;
    // }

    assert(BufMgr().sync_all());
    assert(TblMgr().close_all_tables());

    LogMgr().truncate_log();
}

void Recovery::analyse()
{
    f_log_msg_ << "[ANALYSIS] Analysis pass start\n";

    const lsn_t next_lsn = LogMgr().next_lsn();
    for (lsn_t lsn = LogMgr().base_lsn(); lsn < next_lsn;)
    {
        Log log = LogMgr().read_log(lsn);

        if (log.type() == LogType::BEGIN)
        {
            xacts_[log.xid()] = false;
            losers_[log.xid()] = NULL_LSN;
        }
        else if (log.type() == LogType::COMMIT ||
                 log.type() == LogType::ROLLBACK)
        {
            xacts_[log.xid()] = true;
            losers_.erase(log.xid());
        }
        else if (Log::HasRecord(log.type()))
        {
            if (!TblMgr().is_open(log.table_id()))
                assert(TblMgr().open_table(std::string("DATA") +
                                           std::to_string(log.table_id())));

            losers_[log.xid()] = lsn;
        }

        logs_[lsn] = log;

        lsn += log.size();
    }

    f_log_msg_ << "[ANALYSIS] Analysis success. Winner:";
    for (const auto& pr : xacts_)
        if (pr.second)
            f_log_msg_ << ' ' << pr.first;

    f_log_msg_ << ", Loser:";
    for (const auto& pr : xacts_)
        if (!pr.second)
            f_log_msg_ << ' ' << pr.first;
    f_log_msg_ << '\n';

    f_log_msg_.flush();
}

bool Recovery::redo()
{
    f_log_msg_ << "[REDO] Redo pass start\n";

    const lsn_t next_lsn = LogMgr().next_lsn();
    for (lsn_t lsn = LogMgr().base_lsn(); lsn < next_lsn;)
    {
        Log& log = logs_[lsn];

        f_log_msg_ << "LSN " << lsn + log.size() << " ";

        switch (log.type())
        {
            case LogType::BEGIN:
                f_log_msg_ << "[BEGIN] Transaction id " << log.xid();
                break;

            case LogType::COMMIT:
                f_log_msg_ << "[COMMIT] Transaction id " << log.xid();
                break;

            case LogType::ROLLBACK:
                f_log_msg_ << "[ROLLBACK] Transaction id " << log.xid();
                break;

            case LogType::UPDATE:
            case LogType::COMPENSATE: {
                Table* table = TblMgr().get_table(log.table_id()).value();
                CHECK_FAILURE(buffer(
                    [&](Page& page) {
                        if (page.header().page_lsn < lsn)
                        {
                            if (log.type() == LogType::UPDATE)
                            {
                                f_log_msg_ << "[UPDATE] Transaction id "
                                           << log.xid() << " redo apply";
                            }
                            else
                            {
                                f_log_msg_ << "[CLR] next undo lsn "
                                           << log.next_undo_lsn() + logs_[log.next_undo_lsn()].size();
                            }

                            const HierarchyID hid(
                                log.table_id(), log.pagenum(),
                                (log.offset() - 8 - PAGE_HEADER_SIZE) /
                                    PAGE_DATA_SIZE);

                            page.header().page_lsn = lsn;
                            memcpy(page.data()[hid.offset].value,
                                   log.new_data(), log.length());

                            page.mark_dirty();
                        }
                        else
                        {
                            f_log_msg_ << "[CONSIDER-REDO] Transaction id "
                                       << log.xid();
                        }
                    },
                    *table, log.pagenum()));
            }

            default:
                break;
        }

        f_log_msg_ << std::endl;

        if (mode_ == RecoveryMode::REDO_CRASH && --log_num_ == 0)
        {
            f_log_msg_.flush();
            return false;
        }

        lsn += log.size();
    }

    f_log_msg_ << "[REDO] Redo pass end" << std::endl;
    f_log_msg_.flush();

    return true;
}

bool Recovery::undo()
{
    f_log_msg_ << "[UNDO] Undo pass start\n";

    int no_nil_loser = 0;
    std::unordered_map<xact_id, lsn_t> att;
    for (auto& pr : losers_)
    {
        if (pr.second != NULL_LSN)
            ++no_nil_loser;
    }

    while (no_nil_loser > 0)
    {
        xact_id nexttrans;
        lsn_t nextentry = NULL_LSN;
        {
            for (auto& loser : losers_)
            {
                if (nextentry <= loser.second)
                {
                    nexttrans = loser.first;
                    nextentry = loser.second;
                }
            }
        }

        Log& log = logs_[nextentry];

        if (log.type() == LogType::COMPENSATE)
        {
            f_log_msg_ << "LSN " << log.lsn() + log.size()
                       << " [CLR] next undo lsn " << log.next_undo_lsn() + logs_[log.next_undo_lsn()].size();

            if ((losers_[nexttrans] = log.next_undo_lsn()) == NULL_LSN)
                --no_nil_loser;
        }
        else if (log.type() == LogType::UPDATE)
        {
            Table* table = TblMgr().get_table(log.table_id()).value();
            CHECK_FAILURE(buffer(
                [&](Page& page) {
                    if (page.header().page_lsn >= log.lsn())
                    {
                        const HierarchyID hid(
                            log.table_id(), log.pagenum(),
                            (log.offset() - 8 - PAGE_HEADER_SIZE) /
                                PAGE_DATA_SIZE);

                        memcpy(page.data()[hid.offset].value, log.old_data(),
                               log.length());

                        att[log.xid()] = page.header().page_lsn =
                            LogMgr().log_compensate(
                                log.xid(), att[log.xid()], hid,
                                PAGE_DATA_VALUE_SIZE, log.new_data(),
                                log.old_data(), log.last_lsn());

                        page.mark_dirty();
                    }
                },
                *table, log.pagenum(), false));

            if ((losers_[nexttrans] = log.last_lsn()) == NULL_LSN)
                --no_nil_loser;

            f_log_msg_ << "LSN " << log.lsn() + log.size()
                       << " [UPDATE] Transaction id " << log.xid()
                       << " undo apply";
        }
        else if (log.type() == LogType::BEGIN)
        {
            LogMgr().log_rollback(log.xid(), att[log.xid()]);

            att.erase(log.xid());
            losers_.erase(log.xid());
            --no_nil_loser;
        }
        else if (log.type() == LogType::ROLLBACK)
        {
            losers_[log.xid()] = log.last_lsn();
        }

        if (mode_ == RecoveryMode::UNDO_CRASH && --log_num_ == 0)
        {
            f_log_msg_.flush();
            return false;
        }

        f_log_msg_ << std::endl;
    }

    f_log_msg_ << "[UNDO] Undo pass end" << std::endl;
    f_log_msg_.flush();

    LogMgr().force();

    return true;
}