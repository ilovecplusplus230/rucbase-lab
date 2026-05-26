/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    rid_ = {.page_no = RM_FIRST_RECORD_PAGE, .slot_no = -1};
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
// 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    auto page_handle = file_handle_->fetch_page_handle(rid_.page_no);
    int next_slot = Bitmap::next_bit(
        true, page_handle.bitmap, file_handle_->file_hdr_.num_records_per_page,
        rid_.slot_no);

    if (next_slot < file_handle_->file_hdr_.num_records_per_page) {
        rid_.slot_no = next_slot;  // 在当前页查找下一个有效记录
    } else {
        // 查找后续页面
        rid_.page_no++;  // 1. 切换到下一页
        while (rid_.page_no <
               file_handle_->file_hdr_.num_pages) {  // 2. 遍历所有页
            page_handle = file_handle_->fetch_page_handle(
                rid_.page_no);  // 3. 获取页面句柄
            int first_slot = Bitmap::first_bit(
                true, page_handle.bitmap,  // 4. 查找第一个有效槽位
                file_handle_->file_hdr_.num_records_per_page);
            if (first_slot < file_handle_->file_hdr_
                                 .num_records_per_page) {  // 5. 判断是否找到
                rid_.slot_no = first_slot;                 // 6. 更新记录位置
                return;                                    // 7. 找到后立即返回
            }
            rid_.page_no++;  // 8. 继续检查下一页
        }
        rid_ = Rid{RM_NO_PAGE, -1};  // 扫描结束
    }
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // 修改返回值
    return rid_.page_no == RM_NO_PAGE;

    return false;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}