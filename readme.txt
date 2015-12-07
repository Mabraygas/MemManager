/**
 * @brief    内存池
 * @author   mabraygas
 * @version  2.0
 * @lastedit 2015-11-24
 * @usage    1. pages_init(space, factor, prealloc);
 *				space为内存池的总容量, 单位为MB
 *				factor是内存池slabs容量的增长参数
 *				prealloc为是否预先开辟空间标志
 *				其中, factor、prealloc具有默认参数，可以不设置
 *				如：pages_init(100); /* 100MB pool space */
 *			 2. 申请内存与释放内存：
 *				int* ptr = New<int>(100); /* equals to int* ptr = new int[100]; */
 *				Delete(ptr); /* equals to delete[] ptr; */
 *				支持自定义类型的空间申请与释放
 *			 3. g++ -std=c++0x ...
 * @advantage
 *			 1. 适用于大量的小块内存申请与释放的场景中, 可以减少内存碎片的产生。大块内存不适用。
 * @disadvantage
 *			 1. New的过程暂时只支持调用默认构造函数, 暂不支持传递参数的构造函数。
 *			 2. 大块内存场景下不适用。
 */
