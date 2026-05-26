/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"

#include "common/context.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    // 1. 获取指定记录所在的page handle
    // 2. 初始化一个指向RmRecord的指针（赋值其内部的data和size）
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 调用 fetch_page_handle() 获取指定页面的 page handle，并封装为 RmPageHandle 对象
    char* slot = page_handle.get_slot(rid.slot_no);
    // 通过 get_slot 计算目标槽位的物理地址：槽位地址 = slots起始地址 + slot_no * record_size
    auto record = std::make_unique<RmRecord>(file_hdr_.record_size);
    memcpy(record->data, slot, file_hdr_.record_size);
    // 根据文件头中定义的 record_size 分配内存，并将槽位数据复制到 RmRecord 中
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    // 操作完成后解除页面锁定(pin_count--)，如果页面被修改了则传入true，否则传入false
    return record;
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {char*} buf 要插入的记录的数据
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 *
 * 实现步骤：
 * 1. 获取一个有空闲槽位的页面（优先复用空闲页，无则创建新页）
 * 2. 在页面中查找第一个空间键位
 * 3. 更新位图标记槽位已使用
 * 4. 讲记录数据复制到槽位中
 * 5. 更新页面记录计数
 * 6. 如果页面变满，更新空闲页链表
 * 7. 返回新记录的RID
 */
Rid RmFileHandle::insert_record(char* buf, Context* context) {
    // 1. 获取当前未满的page handle
    RmPageHandle page_handle = create_page_handle();
    // 2. 在page handle中找到空闲slot位置
    int slot_no = Bitmap::first_bit(false,page_handle.bitmap,file_hdr_.num_records_per_page);
    // 3.设置位图标记槽位已使用
    Bitmap::set(page_handle.bitmap, slot_no);
    // 4. 将buf复制到空闲slot位置
    memcpy(page_handle.get_slot(slot_no),buf,file_hdr_.record_size);
    // 5. 更新页面记录计数
    page_handle.page_hdr->num_records++;
    // 6. 如果页面变满，更新空闲页链表
    if(page_handle.page_hdr->num_records == file_hdr_.num_records_per_page){
        // 从空闲页链表中移除该页面
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        // 当前页标记为无后续空闲页
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    // 7. 构造新记录的RID
    Rid rid{page_handle.page->get_page_id().page_no, slot_no};
    // 8. 解除页面锁定
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);

    return rid;
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    // 1. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 2. 记录删除前页面是否已满
    bool was_full = (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page);
    // 3. 设置位图标记槽位未使用
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    // 4. 更新页面记录计数
    page_handle.page_hdr->num_records--;
    // 5. 如果页面从满变为未满，更新空闲页链表
    if(was_full){
        release_page_handle(page_handle);
    }
    // 6. 解除页面锁定
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}


/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {char*} buf 新记录的数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
    // 1. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 2. 更新记录数据
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    // 3. 解除页面锁定
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * 以下函数为辅助函数，仅提供参考，可以选择完成如下函数，也可以删除如下函数，在单元测试中不涉及如下函数接口的直接调用
*/
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    // 1. 检查页面号范围
    if(page_no < 0 || page_no >= file_hdr_.num_pages){
        throw PageNotExistError("", page_no);
    }
    // 构造页面ID（文件描述符+页面号）
    PageId page_id = {.fd = fd_, .page_no = page_no};
    // 2. 从缓冲池获取页面
    Page* page = buffer_pool_manager_->fetch_page(page_id);
    if (!page) {
        throw PageNotExistError("Failed to fetch page", page_no);
    }
    // 3. 构造页面句柄（自动解析页面头、位图和槽位）
    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    // 1.使用缓冲池来创建一个新page
    // 2.更新page handle中的相关信息
    // 3.更新file_hdr_
    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page* new_page = buffer_pool_manager_->new_page(&new_page_id);
    if (!new_page) {
        throw InternalError("No free pages available");
    }
    // 初始化页面头
    RmPageHdr page_hdr{};
    page_hdr.next_free_page_no = -1;
    page_hdr.num_records = 0;
    memcpy(new_page->get_data(), &page_hdr, sizeof(RmPageHdr));
    // 初始化位图全为0
    char* bitmap = new_page->get_data() + sizeof(RmPageHdr);
    Bitmap::init(bitmap,file_hdr_.bitmap_size);
    // 更新文件头信息
    file_hdr_.num_pages++;
    disk_manager_->write_page(fd_,RM_FILE_HDR_PAGE,(char*)&file_hdr_,sizeof(file_hdr_));
    return RmPageHandle(&file_hdr_, new_page);
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    // 1. 判断file_hdr_中是否还有空闲页
    // 1.没有空闲页可用
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        return create_new_page_handle();
    }
    // 2.有空闲页可用，直接获取
    RmPageHandle page_handle = fetch_page_handle(file_hdr_.first_free_page_no);
    // 更新file_hdr_中的first_free_page_no为当前页的下一个空闲页
    file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
    // 将更新后的文件头立即持久化到硬盘
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char*)&file_hdr_, sizeof(file_hdr_));
    return page_handle;
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle&page_handle) {
    // 当page从已满变成未满，考虑如何更新：
    // 1. page_handle.page_hdr->next_free_page_no
    // 2. file_hdr_.first_free_page_no
    if (page_handle.page_hdr->next_free_page_no == RM_NO_PAGE) {
        // 将当前页面插入空闲链表头部
        page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
        file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
        // 将更新后的文件头立即持久化到硬盘
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char*)&file_hdr_, sizeof(file_hdr_));
    }
}