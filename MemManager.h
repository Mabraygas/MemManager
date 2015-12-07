#ifndef MEM_MANAGER_H_
#define MEM_MANAGER_H_
/**
 * @file   MemManager.h
 * @brief  内存池
 * @author mabraygas
 * @ver    1.0
 * @date   2015-09-25
 */
#include <new>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <type_traits>

#define POWER_SMALLEST 1
#define POWER_LARGEST  200
#define PAGE_ALIGN 8
#define PAGE_MAX_CLASS_NUM POWER_LARGEST
static  int page_class_num; //the real page class num

//memory align of 8 bytes
#define ROUND_UP(size) { \
	size = (size + PAGE_ALIGN - 1) & ~(PAGE_ALIGN - 1); \
}

static const size_t MM_MIN_SIZE = 1; /* 1 byte */
static const size_t MM_MAX_SIZE = 1 * 1024 * 1024; /* 1MB */
static pthread_mutex_t page_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
	uint8_t page_classid;
	size_t alloc_num;
} item;

typedef struct {
	uint32_t chunk_size;   //chunk size of this pageclass
	uint32_t perpage;      //chunk num per page

	void**   freelist;     //free slots list of this pageclass
	uint32_t free_total;   //total num of free slots
	uint32_t free_curr;    //use num of free slots

	void*    end_page_ptr; //pointer to the last page(newest page)
	uint32_t end_page_free;//free chunk num of end page

	uint32_t page_total;   //page use num of this pageclass
	void**   page_list;    //page list of this pageclass
	uint32_t page_size;    //total size of the page list

} pageclass_t;

static pageclass_t pageclass[PAGE_MAX_CLASS_NUM];

static void* mem_pool = NULL; 		//point to begin of the mempool
static void* mem_avaliable = NULL;  //point to avaliable position of the mempool

static uint64_t memalloc = 0;       //total size of the mempool
static uint64_t memuse = 0;     	//mempool already use

//Forward Declaration
static void* mem_alloc(size_t size);
static void  mem_free(void* ptr, size_t size);

static void* do_mem_alloc(size_t size);
static void  do_mem_free(void* pointer, size_t size);

static void prealloc_page(int num);
static int alloc_newpage(const int pageclass_id);

static int grow_page_list(const int pageclass_id);
static void* memory_alloc(size_t size);

static int get_page_id(size_t size);

//usage: T* pt = New<T>(count);
template<typename T>
T* New(size_t size) {
	if(size <= 0) { return NULL; }
	size_t total_size = sizeof(T) * size + sizeof(item);
	void* pointer = NULL;
	//choose alloc type
	if(total_size > MM_MAX_SIZE) {
		pointer = malloc(total_size);
	}else {
		pointer = mem_alloc(total_size);
	}
	if(NULL == pointer) { return NULL; }

	//whether is plain of data
	bool _Is_pod = std::is_pod<T>::value;
	if(!_Is_pod) {
		//has non-trivial constructor
		void* construct_ptr = (char*)pointer + sizeof(item);
		for(size_t i = 0; i < size; i ++ ) {
			//placement new
			new(construct_ptr) T();
			construct_ptr = (char*)construct_ptr + sizeof(T);
		}
	}
	((item*)pointer)->alloc_num = size;
	return (T*)((char*)pointer + sizeof(item));
}

//usage: Delete(pt, count);
template<typename T>
void Delete(T* ptr) {
	if(ptr == NULL) { return; }
	size_t count = ((item*)((char*)ptr - sizeof(item)))->alloc_num;
	if(count <= 0) { return; }

	size_t total_size = sizeof(T) * count + sizeof(item);

	//whether is plain of data
	bool _Is_pod = std::is_pod<T>::value;
	if(!_Is_pod) {
		//has non-trivial destructor
		void* destruct_ptr = ptr;
		for(size_t i = 0; i < count; i ++ ) {
			//placement delete
			((T*)destruct_ptr)->~T();
			destruct_ptr = (char*)destruct_ptr + sizeof(T);
		}
	}
	if(total_size > MM_MAX_SIZE) {
		free((char*)ptr - sizeof(item));
	}else {
		mem_free((char*)ptr - sizeof(item), count);
	}
}

static void pages_init(uint32_t memlimit /* MB */, const double factor = 1.2, bool prealloc = 1) {
	//if define prealloc, minimum memory = 15MB
	if(prealloc) {
		if(memlimit < 15ul) {
			memlimit = 15ul;
		}
	}
	uint64_t memsize = 1ul * 1024 * 1024 * memlimit;
	//index begin from 1, 0 means not belong to a pageclass
	int index = POWER_SMALLEST;
	size_t size = sizeof(item) + MM_MIN_SIZE;
	if(prealloc) {
		if(NULL != mem_pool) {
			//memory already set up
			return;
		}
		mem_pool = malloc(memsize);
		if(NULL == mem_pool) {
			fprintf(stderr, "prealloc mempool failed! %s %d\n", __FILE__, __LINE__);
		}
		mem_avaliable = mem_pool;

		memalloc = memsize;
		memuse = 0;
	
		//memset(mem_pool, 0x00, memsize);
	}

	memset(pageclass, 0x00, sizeof(pageclass_t) * PAGE_MAX_CLASS_NUM);

	ROUND_UP(size);
	for(; index < POWER_LARGEST && size < MM_MAX_SIZE; index ++ ) {
		pageclass[index].chunk_size = size;
		pageclass[index].perpage = MM_MAX_SIZE / size;
		
		size = (size_t)((double)(size) * factor);
		ROUND_UP(size);
	}
	page_class_num = index;
	pageclass[page_class_num].chunk_size = MM_MAX_SIZE;
	pageclass[page_class_num].perpage = 1;

#ifndef DONT_PREALLOC_PAGE
	prealloc_page(page_class_num);
#endif
}

#ifndef DONT_PREALLOC_PAGE
static void prealloc_page(int num) {
	for(int i = POWER_SMALLEST; i <= num && i < POWER_LARGEST; i ++ ) {
		alloc_newpage(i);
	}
}
#endif

static int alloc_newpage(const int pageclass_id) {
	pageclass_t* ptr = &pageclass[pageclass_id];
	size_t size = ptr->chunk_size * ptr->perpage;

	void* new_page;
	if((mem_pool && memuse + size > memalloc && ptr->page_total) 
		|| (grow_page_list(pageclass_id) == 0) 
		|| ((new_page = memory_alloc(size)) == NULL)) {
		//fail if outof mempool or grow_page_list fail or alloc space for newpage fail
		return 0;
	}

	memset(new_page, 0x00, size);
	ptr->end_page_ptr = new_page;
	ptr->end_page_free= ptr->perpage;

	ptr->page_list[ptr->page_total++] = new_page;

	//success
	return 1;
}

static int grow_page_list(const int pageclass_id) {
	pageclass_t* ptr = &pageclass[pageclass_id];

	if(ptr->page_size == ptr->page_total) {
		//need grow
		uint32_t new_size = ptr->page_size == 0 ? 16 : 2 * ptr->page_size;
		void** new_list = (void **)realloc(ptr->page_list, new_size * sizeof(void *));
		if(NULL == new_list) {
			//fail
			return 0;
		}
		ptr->page_list = new_list;
		ptr->page_size = new_size;
	}
	//grow success or neednot to grow
	return 1;
}

static void* memory_alloc(size_t size) {
	void* ret = NULL;
	//if prealloc a mempool, check from the pool
	if(mem_pool) {
		ret = mem_avaliable;
		if(memuse + size > memalloc) {
			return NULL;
		}
		ROUND_UP(size);
		if(memuse + size > memalloc) {
			mem_avaliable = (char *)mem_avaliable + (memalloc - memuse);
			memuse = memalloc;
		}else {
			mem_avaliable = (char *)mem_avaliable + size;
			memuse += size;
		}
		return ret;
	}
	//use system malloc
	ret = malloc(size);
	return ret;
}

static void* do_mem_alloc(size_t size) {
	void* ret = NULL;

	int id = get_page_id(size);
	//valid value: [1, page_class_num]
	if(id < POWER_SMALLEST || id > page_class_num) {
		fprintf(stderr, "get pageid error while do_mem_alloc, pageid = %d\n", id);
		return NULL;
	}

	pageclass_t* ptr = &pageclass[id];
	//check if pageclass id has free slot or has empty chunk or can alloc a new page
	if((ptr->end_page_ptr == 0) 
		&& (ptr->free_curr == 0)
		&& (alloc_newpage(id) == 0)) {
		return NULL;
	}
	if(ptr->free_curr != 0) {
		ret =  ptr->freelist[--ptr->free_curr];
	}else if(ptr->end_page_ptr != 0) {
		ret = ptr->end_page_ptr;
		if(--ptr->end_page_free == 0) {
			ptr->end_page_ptr = 0;
		}else {
			ptr->end_page_ptr = (char *)ptr->end_page_ptr + ptr->chunk_size;
		}
	}
	if(ret != NULL) {
		((item*)ret)->page_classid = id;
	}
	return ret;
}

static void do_mem_free(void* pointer, size_t size) {
	item* p = (item *)pointer;
	int id = p->page_classid;
	if(id < POWER_SMALLEST || id > page_class_num) {
		fprintf(stderr, "get pageid error while do_mem_free, pageid = %d\n", id);
		return;
	}
	
	pageclass_t* ptr = &pageclass[id];
	//whether need append class_id's freelist
	if(ptr->free_curr == ptr->free_total) {
		uint32_t new_size = ptr->free_total == 0 ? 16 : 2 * ptr->free_total;
		void** new_free_list = (void **)realloc(ptr->freelist, new_size * sizeof(void *));
		if(NULL != new_free_list) {
			ptr->freelist = new_free_list;
			ptr->free_total = new_size;
		}else {
			return;
		}
	}
	ptr->freelist[ptr->free_curr++] = pointer;
}

static int get_page_id(size_t size) {
	if(size == 0) { return 0; }
	int index = POWER_SMALLEST;
	for(; index <= page_class_num && index < POWER_LARGEST; index ++ ) {
		if(pageclass[index].chunk_size >= size) {
			return index;
		}
	}
	//fail
	return 0;
}

static void* mem_alloc(size_t size) {
	void* ret;
	pthread_mutex_lock(&page_lock);
	ret = do_mem_alloc(size);
	pthread_mutex_unlock(&page_lock);
	return ret;
}

static void mem_free(void* ptr, size_t size) {
	pthread_mutex_lock(&page_lock);
	do_mem_free(ptr, size);
	pthread_mutex_unlock(&page_lock);
}

#endif //MEM_MANAGER_H_

// vim: ts=4 sw=4 nu

