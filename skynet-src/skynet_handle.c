#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4
#define MAX_SLOT_SIZE 0x40000000

struct handle_name { //ctx的handle与name对应关系
	char * name;
	uint32_t handle;
};

struct handle_storage { //管理所有ctx结构
	struct rwlock lock; //读写锁

	uint32_t harbor; //每个skynet节点可配置一个唯一harbor，用于与其他节点组网
	uint32_t handle_index; //下次从handle_index位置开始查找空的位置
	int slot_size; //已分配的ctx的容量
	struct skynet_context ** slot; //slot_size容量的数组，每一项指向一个ctx
	
	int name_cap; //已分配ctx名字的容量
	int name_count; //已用的名字
	struct handle_name *name; //name_cap容量的数组，每一项是一个handle_name
};

static struct handle_storage *H = NULL;

//当创建ctx时，会注册一个唯一的handle
uint32_t
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);
	
	for (;;) {
		int i;
		for (i=0;i<s->slot_size;i++) {
			uint32_t handle = (i+s->handle_index) & HANDLE_MASK; //24位，所以最多有2^24=4M个ctx
			int hash = handle & (s->slot_size-1);
			if (s->slot[hash] == NULL) { //找到一个空的位置存放ctx
				s->slot[hash] = ctx;
				s->handle_index = handle + 1; //下次从handle+1出开始查找空位置

				rwlock_wunlock(&s->lock);

				handle |= s->harbor; //高8位为harbor id
				return handle;
			}
		}
		assert((s->slot_size*2 - 1) <= HANDLE_MASK);
        //没有空的位置，扩充容量，然后把数据迁移到新数据里
		struct skynet_context ** new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *));
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));
		for (i=0;i<s->slot_size;i++) {
			int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);
			assert(new_slot[hash] == NULL);
			new_slot[hash] = s->slot[i];
		}
		skynet_free(s->slot);
		s->slot = new_slot;
		s->slot_size *= 2;
	}
}

int
skynet_handle_retire(uint32_t handle) { //当ctx delete后，回收handle
	int ret = 0;
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock); //写加锁，期间其他线程不能读写handle仓库

	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];

	if (ctx != NULL && skynet_context_handle(ctx) == handle) {
		s->slot[hash] = NULL; //handle所在位置置空，供下一个ctx使用
		ret = 1;
		int i;
		int j=0, n=s->name_count;
		for (i=0; i<n; ++i) { //回收跟handle对应的name
			if (s->name[i].handle == handle) {
				skynet_free(s->name[i].name);
				continue;
			} else if (i!=j) {
				s->name[j] = s->name[i];
			}
			++j;
		}
		s->name_count = j;
	} else {
		ctx = NULL;
	}

	rwlock_wunlock(&s->lock);

	if (ctx) {
		// release ctx may call skynet_handle_* , so wunlock first.
		skynet_context_release(ctx);
	}

	return ret;
}

void 
skynet_handle_retireall() {
	struct handle_storage *s = H;
	for (;;) {
		int n=0;
		int i;
		for (i=0;i<s->slot_size;i++) {
			rwlock_rlock(&s->lock);
			struct skynet_context * ctx = s->slot[i];
			uint32_t handle = 0;
			if (ctx)
				handle = skynet_context_handle(ctx);
			rwlock_runlock(&s->lock);
			if (handle != 0) {
				if (skynet_handle_retire(handle)) {
					++n;
				}
			}
		}
		if (n==0)
			return;
	}
}

struct skynet_context * 
skynet_handle_grab(uint32_t handle) { //通过handle获得对应的ctx
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	rwlock_rlock(&s->lock);

	uint32_t hash = handle & (s->slot_size-1);//找到位于数组中的位置
	struct skynet_context * ctx = s->slot[hash];
	if (ctx && skynet_context_handle(ctx) == handle) {
		result = ctx;
		skynet_context_grab(result);
	}

	rwlock_runlock(&s->lock);

	return result;
}

uint32_t 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);

	uint32_t handle = 0;

	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) { //二分查找name对应的handle
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);

	return handle;
}

static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {
	if (s->name_count >= s->name_cap) {
        //数组满时，扩充容量(原来的2倍)，然后迁移数据
		s->name_cap *= 2;
		assert(s->name_cap <= MAX_SLOT_SIZE);
		struct handle_name * n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}
		skynet_free(s->name);
		s->name = n;
	} else {
		int i;
        //在before位置插入一个数据，把before之后数据向后移
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}

//给ctx注册一个名字，放入s->name里，s->name按handle_name->name升序排序
static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) { //二分查找可插入的位置，保证插入后有序
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			return NULL;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	char * result = skynet_strdup(name);

	_insert_name_before(s, result, handle, begin);

	return result;
}

//给handle注册一个name
const char * 
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);

	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}

void 
skynet_handle_init(int harbor) { //初始化,harbor在配置文件中可配置
	assert(H==NULL);
	struct handle_storage * s = skynet_malloc(sizeof(*H));
	s->slot_size = DEFAULT_SLOT_SIZE;
    //给slot分配slot_size个ctx内存，
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *));
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));

	rwlock_init(&s->lock); //读写锁
	// reserve 0 for system
    // harbor取8位，所以最多可组网2^8=255 skynet节点
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;
	s->handle_index = 1;
	s->name_cap = 2;
	s->name_count = 0;
    //给name分配name_cap个handle_name内存
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}

