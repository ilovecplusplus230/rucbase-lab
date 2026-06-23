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

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();
    }

    void nextTuple() override {
        // 右表未结束：仅移动右表指针（内层循环）。
        // 右表结束：
        //  移动左表指针（外层循环）。
        //  若左表结束，标记 isend = true。
        //  否则重置右表到起始位置（开始新一轮内层循环）。
        // 通过模拟嵌套循环（左表为外层，右表为内层）实现连接。
        if (!right_->is_end()) {
            right_->nextTuple();
        }

        while (!left_->is_end()) {
            auto left_rec = left_->Next();
            while (!right_->is_end()) {
                auto right_rec = right_->Next();
                if (eval_conds(left_rec.get(), right_rec.get(), fed_conds_,
                               cols_)) {
                    return;
                }
                right_->nextTuple();
            }

            left_->nextTuple();
            if (left_->is_end()) break;

            right_->beginTuple();
        }
        isend = true;
    }

    std::unique_ptr<RmRecord> Next() override {
        // 若满足条件，将左右元组拼接成新记录返回。
        while (!isend) {
            std::unique_ptr<RmRecord> left_rec = left_->Next();
            std::unique_ptr<RmRecord> right_rec = right_->Next();

            if (eval_conds(left_rec.get(), right_rec.get(), fed_conds_,
                           cols_)) {
                std::unique_ptr<RmRecord> join_rec =
                    std::make_unique<RmRecord>(len_);
                memcpy(join_rec->data, left_rec->data, left_->tupleLen());
                memcpy(join_rec->data + left_->tupleLen(), right_rec->data,
                       right_->tupleLen());
                return join_rec;
            }

            nextTuple();
        }

        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return left_->is_end(); }
    
    std::string getType() override { return "NestedLoopJoinExecutor"; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    private:
    bool eval_cond(const RmRecord *lhs_rec, const RmRecord *rhs_rec, const Condition &cond, const std::vector<ColMeta> &rec_cols) {
        auto left_col = left_->get_col(left_->cols(), cond.lhs_col);
        char *left_val = lhs_rec->data + left_col->offset;

        auto right_col = right_->get_col(right_->cols(), cond.rhs_col);
        char *right_val = rhs_rec->data + right_col->offset;

        int cmp_result =
            ix_compare(left_val, right_val, right_col->type, right_col->len);
        switch (cond.op) {
            case OP_EQ:
                return cmp_result == 0;
            case OP_NE:
                return cmp_result != 0;
            case OP_LT:
                return cmp_result < 0;
            case OP_GT:
                return cmp_result > 0;
            case OP_LE:
                return cmp_result <= 0;
            case OP_GE:
                return cmp_result >= 0;
            default:
                return false;
        }
    }

    bool eval_conds(const RmRecord *lhs_rec, const RmRecord *rhs_rec, const std::vector<Condition> &conds, const std::vector<ColMeta> &rec_cols) {
        return std::all_of(conds.begin(), conds.end(),
            [&](const Condition &cond) { return eval_cond(lhs_rec, rhs_rec, cond, rec_cols); }
        );
    }
};