/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"

/**
 * @description: 从free_list或replacer中得到可淘汰帧页的 *frame_id
 * @return {bool} true: 可替换帧查找成功 , false: 可替换帧查找失败
 * @param {frame_id_t*} frame_id 帧页id指针,返回成功找到的可替换帧id
 */
bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    // 1 使用BufferPoolManager::free_list_判断缓冲池是否已满需要淘汰页面
    // 1.1 未满获得frame
    if (!free_list_.empty()) {
        // 如果空闲列表不为空，说明缓冲池还有空闲帧可用
        // 从空闲列表头部获取一个空闲帧号
        *frame_id = free_list_.front();
        // 将该帧号从空闲列表中移除
        free_list_.pop_front();
        // 返回成功找到可替换帧
        return true;
    }
    // 1.2 已满使用lru_replacer中的方法选择淘汰页面
    // 如果空闲列表为空，说明缓冲池已满
    // 调用replacer的victim方法选择一个可淘汰的帧
    // 该方法会根据LRU策略选择最近最少使用的帧进行淘汰
    return replacer_->victim(frame_id);
}

/**
 * @description: 更新页面数据, 如果为脏页则需写入磁盘，再更新为新页面，更新page元数据(data, is_dirty, page_id)和page table
 * @param {Page*} page 写回页指针
 * @param {PageId} new_page_id 新的page_id
 * @param {frame_id_t} new_frame_id 新的帧frame_id
 */
// void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    // Todo:
    // 1 如果是脏页，写回磁盘，并且把dirty置为false
    // 2 更新page table
    // 3 重置page的data，更新page id

// }

/**
 * @description: 从buffer pool获取需要的页。
 *              如果页表中存在page_id（说明该page在缓冲池中），并且pin_count++。
 *              如果页表不存在page_id（说明该page在磁盘中），则找缓冲池victim page，将其替换为磁盘中读取的page，pin_count置1。
 * @return {Page*} 若获得了需要的页则将其返回，否则返回nullptr
 * @param {PageId} page_id 需要获取的页的PageId
 */
Page* BufferPoolManager::fetch_page(PageId page_id) {
    // 1.     从page_table_中搜寻目标页
    std::lock_guard<std::mutex> guard(latch_);
    // 使用std::lock_guard来管理锁的生命周期，确保在函数结束时自动释放锁
    auto it = page_table_.find(page_id);
    // 1.1    若目标页有被page_table_记录，则将其所在frame固定(pin)，并返回目标页。
    if (it != page_table_.end()) {
        // 如果目标页在页表中存在，说明该页已经在缓冲池中
        // 获取该页所在的帧号
        frame_id_t frame_id = it->second;
        // 从缓冲池中获取该页的指针
        Page* page = &pages_[frame_id];
        // 将该页固定(pin)，即增加pin_count_
        page->pin_count_++;
        // 将目标页的pin_count+1，表示该页正在被使用
        replacer_->pin(frame_id);
        // 返回目标页的指针
        return page;
    }
    // 1.2    否则，尝试调用find_victim_page获得一个可用的frame，若失败则返回nullptr
    frame_id_t frame_id;
    //定义用于存储找到的可替换帧页id的变量
    if (!find_victim_page(&frame_id)) {
        // 如果无法找到可替换的帧页，说明缓冲池已满且没有可替换的帧页可用
        // 返回nullptr表示获取页失败
        return nullptr;
    }
    Page* victim_page = &pages_[frame_id];
    // 获取到的可替换帧页指针
    // 2.若获得的可用frame存储的为dirty page，则须调用write_page将page写回到磁盘
    if (victim_page->is_dirty_) {
        // 如果可替换的帧页是脏页，说明该页的数据已经被修改但尚未写回磁盘
        // 调用disk_manager_的write_page方法将该页写回磁盘
        disk_manager_->write_page(victim_page->get_page_id().fd, 
                                  victim_page->get_page_id().page_no, 
                                  victim_page->get_data(),PAGE_SIZE);
        // 将该页的is_dirty_标志置为false，表示该页已经被写回磁盘，不再是脏页
        victim_page->is_dirty_ = false;
    }
    // 3.     调用disk_manager_的read_page读取目标页到frame
    disk_manager_->read_page(page_id.fd, page_id.page_no, victim_page->data_, PAGE_SIZE);
    // 4.     固定目标页，更新pin_count_和page表
    page_table_.erase(victim_page->get_page_id()); //从页表移除旧的页
    page_table_[page_id] = frame_id; //将新的页id和帧号添加到页表中
    victim_page->id_ = page_id; //更新frame中页的id为新的page_id
    victim_page->pin_count_ = 1; //将目标页的pin_count置为1，表示该页正在被使用
    // 5.     返回目标页
    replacer_->pin(frame_id); //将新的页所在的帧固定(pin)，即增加pin_count_
    return victim_page; //返回目标页的指针
}

/**
 * @description: 取消固定pin_count>0的在缓冲池中的page
 * @return {bool} 如果目标页的pin_count<=0则返回false，否则返回true
 * @param {PageId} page_id 目标page的page_id
 * @param {bool} is_dirty 若目标page应该被标记为dirty则为true，否则为false
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    // 0. lock latch
    std :: scoped_lock lock{latch_};
    // 1. 尝试在page_table_中搜寻page_id对应的页P
    auto it = page_table_.find(page_id);
    // 1.1 P在页表中不存在 return false
    if (it == page_table_.end()) {
        // 如果目标页在页表中不存在，说明该页不在缓冲池中
        // 返回false表示取消固定失败
        return false;
    }
    // 1.2 P在页表中存在，获取其pin_count_
    frame_id_t frame_id = it->second; //获取目标页所在的帧号
    Page* page = &pages_[frame_id]; //获取目标页的指针
    // 2.1 若pin_count_已经等于0，则返回false
    if (page->pin_count_ <= 0) {
        // 如果目标页的pin_count_已经等于0，说明该页当前没有被固定
        // 返回false表示取消固定失败
        return false;
    }
    // 2.2 若pin_count_大于0，则pin_count_自减一
    page->pin_count_--; //将目标页的pin_count_自减一，表示该页被取消固定
    // 2.2.1 若自减后等于0，则调用replacer_的Unpin
    if (page->pin_count_ == 0) {
        // 如果目标页的pin_count_自减后等于0，说明该页当前没有被固定
        // 调用replacer_的unpin方法将该页所在的帧取消固定，即增加可替换帧的数量
        replacer_->unpin(frame_id);
    }
    // 3 根据参数is_dirty，更改P的is_dirty_
    if(is_dirty){
        // 如果参数is_dirty为true，说明该页应该被标记为脏页
        // 将目标页的is_dirty_标志置为true，表示该页已经被修改但尚未写回磁盘
        page->is_dirty_ = true;
    }
    return true;
}

/**
 * @description: 将目标页写回磁盘，不考虑当前页面是否正在被使用
 * @return {bool} 成功则返回true，否则返回false(只有page_table_中没有目标页时)
 * @param {PageId} page_id 目标页的page_id，不能为INVALID_PAGE_ID
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    // Todo:
    // 0. lock latch
    std :: scoped_lock lock{latch_};
    // 1. 查找页表,尝试获取目标页P
    auto it = page_table_.find(page_id);
    // 1.1 目标页P没有被page_table_记录 ，返回false
    if (it == page_table_.end()) {
        return false;
    }
    // 1.2 目标页P被page_table_记录，获取其frame_id和page指针
    frame_id_t frame_id = it->second; //获取目标页所在的帧号
    Page* page = &pages_[frame_id]; //获取目标页的指针
    // 2. 无论P是否为脏都将其写回磁盘。
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->get_data(), PAGE_SIZE);
    // 3. 更新P的is_dirty_
    page->is_dirty_ = false; //将目标页的is_dirty_标志置为false，表示该页已经被写回磁盘，不再是脏页
    
    return true;
}

/**
 * @description: 创建一个新的page，即从磁盘中移动一个新建的空page到缓冲池某个位置。
 * @return {Page*} 返回新创建的page，若创建失败则返回nullptr
 * @param {PageId*} page_id 当成功创建一个新的page时存储其page_id
 */
Page* BufferPoolManager::new_page(PageId* page_id) {
    // 1.   获得一个可用的frame，若无法获得则返回nullptr
    // 2.   在fd对应的文件分配一个新的page_id
    // 3.   将frame的数据写回磁盘
    // 4.   固定frame，更新pin_count_
    // 5.   返回获得的page
   return nullptr;
}

/**
 * @description: 从buffer_pool删除目标页
 * @return {bool} 如果目标页不存在于buffer_pool或者成功被删除则返回true，若其存在于buffer_pool但无法删除则返回false
 * @param {PageId} page_id 目标页
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    // 1.   在page_table_中查找目标页，若不存在返回true
    // 2.   若目标页的pin_count不为0，则返回false
    // 3.   将目标页数据写回磁盘，从页表中删除目标页，重置其元数据，将其加入free_list_，返回true
    
    return true;
}

/**
 * @description: 将buffer_pool中的所有页写回到磁盘
 * @param {int} fd 文件句柄
 */
void BufferPoolManager::flush_all_pages(int fd) {
    
}