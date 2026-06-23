/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        // 预加载,获取当前表的所有索引元数据 (IndexMeta) 和索引句柄
        // (IxIndexHandle)。
        std::vector<std::pair<IndexMeta*, IxIndexHandle*>> index_handles;
        for (auto& index : tab_.indexes) {
            std::string index_name =
                sm_manager_->get_ix_manager()->get_index_name(
                    tab_name_,
                    index.cols);  // 获取索引名
            auto it = sm_manager_->ihs_.find(index_name);
            if (it != sm_manager_->ihs_.end()) {
                index_handles.emplace_back(&index, it->second.get());
            }
        }

        // 键缓冲区,为每个索引分配内存缓冲区，用于临时存储索引键值。
        std::unordered_map<IndexMeta*, std::vector<char>> key_buffers;
        for (const auto& [index_meta, _] : index_handles) {
            key_buffers[index_meta].resize(index_meta->col_tot_len);
        }

        for (auto& rid : rids_) {  // 遍历所有需要更新的记录
            auto rec = fh_->get_record(rid, context_);

            // 更新
            for (auto& set_clause : set_clauses_) {
                auto lhs_col = tab_.get_col(set_clause.lhs.col_name);
                memcpy(rec->data + lhs_col->offset, set_clause.rhs.raw->data,
                       lhs_col->len);
            }

            // 删除旧索引
            for (auto& [index_meta, ih] : index_handles) {
                auto& key_buf = key_buffers[index_meta];  // 读取记录数据
                int offset = 0;
                // 更新记录字段
                for (size_t j = 0; j < index_meta->col_num; ++j) {
                    memcpy(key_buf.data() + offset,
                           rec->data + index_meta->cols[j].offset,
                           index_meta->cols[j].len);
                    offset += index_meta->cols[j].len;
                }
                // 删除旧索引
                ih->delete_entry(key_buf.data(), context_->txn_);
            }
            // 更新记录
            fh_->update_record(rid, rec->data, context_);

            // 插入新索引
            for (auto& [index_meta, ih] : index_handles) {
                auto& key_buf = key_buffers[index_meta];
                ih->insert_entry(key_buf.data(), rid, context_->txn_);
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};