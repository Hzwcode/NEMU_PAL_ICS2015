#include <stdlib.h>
#include <time.h>
#include "common.h"
#include "burst.h"
#include "misc.h"

//L1cache 存储空间64KB, cache行64B, 8-way set associate, 标志位（valid）, 随机替换算法, write through, no write allocate
#define W_WIDTH1 6
#define Q_WIDTH1 3
#define R_WIDTH1 7
#define F_WIDTH1 (27-W_WIDTH1-Q_WIDTH1-R_WIDTH1)

#define BLOCK_SIZE1 (1 << W_WIDTH1)       //64B
#define BLOCK_NUM1 (1 << Q_WIDTH1)        //8-way set associative
#define GROUP_NUM1 (1 << R_WIDTH1)        //128 groups

//L2cache 存储空间4MB, cache行64B, 16-way set associate, 标志位（valid, dirty）, 随机替换算法, write back, write allocate
#define W_WIDTH2 6
#define Q_WIDTH2 4
#define R_WIDTH2 12
#define F_WIDTH2 (27-W_WIDTH2-Q_WIDTH2-R_WIDTH2)

#define BLOCK_SIZE2 (1 << W_WIDTH2)		  //64B
#define BLOCK_NUM2 (1 << Q_WIDTH2) 		  //16-way set associative
#define GROUP_NUM2 (1 << R_WIDTH2)		  //4096 groups

#define HW_MEM_SIZE (1 << (W_WIDTH1 + Q_WIDTH1 + R_WIDTH1 + F_WIDTH1))

uint32_t dram_read(hwaddr_t, size_t);
void dram_write(hwaddr_t, size_t, uint32_t);
void update_cache(hwaddr_t, void *, size_t);
void update_dram(hwaddr_t, void *, size_t);
void write_dram_with_mask(hwaddr_t addr, void *src, size_t len, uint8_t *mask);

void L2cache_read(hwaddr_t addr,  void *data, uint32_t r, uint32_t q);
void L2cache_write(hwaddr_t addr, void *data, size_t len, uint8_t *mask);

typedef union {
	struct {
		uint32_t w	:W_WIDTH1;
		uint32_t q 	:Q_WIDTH1;
		uint32_t r 	:R_WIDTH1;
		uint32_t f 	:F_WIDTH1;
	};
	uint32_t addr;
}L1cache_addr;

typedef  struct {
	struct {
		uint32_t q 	:Q_WIDTH1;
		uint32_t f 	:F_WIDTH1;
		uint32_t valid	:1;
	};
	uint8_t block[BLOCK_SIZE1];
}L1cache_block;

typedef union {
	struct {
		uint32_t w	:W_WIDTH2;
		uint32_t q 	:Q_WIDTH2;
		uint32_t r 	:R_WIDTH2;
		uint32_t f 	:F_WIDTH2;
	};
	uint32_t addr;
}L2cache_addr;

typedef  struct {
	struct {
		uint32_t q 	:Q_WIDTH2;
		uint32_t f 	:F_WIDTH2;
		uint32_t valid	:1;
		uint32_t dirty	:1;
	};
	uint8_t block[BLOCK_SIZE2];
}L2cache_block;

L1cache_block L1cache[GROUP_NUM1][BLOCK_NUM1];	
L2cache_block L2cache[GROUP_NUM2][BLOCK_NUM2];

void init_L1cache() {
	int i, j;
	for(i = 0; i < GROUP_NUM1; i ++) {
		for(j = 0; j < BLOCK_NUM1; j++){
			L1cache[i][j].valid = 0;
		}
	}
}

void init_L2cache() {
	int i, j;
	for(i = 0; i < GROUP_NUM2; i ++) {
		for(j = 0; j < BLOCK_NUM2; j ++){
			L2cache[i][j].valid = 0;
			L2cache[i][j].dirty = 0;
		}
	}
}

static void L1burst_read(hwaddr_t addr, void *data) {
	Assert(addr < HW_MEM_SIZE, "physical address %x is outside of the physical memory!", addr);

	int i;
	L1cache_addr temp;
	temp.addr = addr & ~BURST_MASK;

	for(i = 0; i < Q_WIDTH1; i++) {
		if(L1cache[temp.r][i].valid == 1 && L1cache[temp.r][i].q == temp.q && L1cache[temp.r][i].f == temp.f) {
			/* burst read */
			memcpy(data, L1cache[temp.r][i].block + temp.w, BURST_LEN);
			return ;
		} 
	}

	for(i = 0;i < Q_WIDTH1; i++) {
		if (L1cache[temp.r][i].valid == 0){
			L1cache[temp.r][i].q = temp.q;
			L1cache[temp.r][i].f = temp.f;
			L1cache[temp.r][i].valid = 1;
			L2cache_read(addr, data, temp.r, i);
			//update_cache(addr, L1cache[temp.r][i].block, BLOCK_SIZE1);
			//memcpy(data, L1cache[temp.r][i].block + temp.w, BURST_LEN);
			return ;
		} 
	}
	srand(time(0));
	i = rand()%BLOCK_NUM1;
	L1cache[temp.r][i].q = temp.q;
	L1cache[temp.r][i].f = temp.f;
	L1cache[temp.r][i].valid = 1;
	L2cache_read(addr, data, temp.r, i);
	//update_cache(addr, L1cache[temp.r][i].block, BLOCK_SIZE1);
	//memcpy(data, L1cache[temp.r][i].block + temp.w, BURST_LEN);
}

static void L1burst_write(hwaddr_t addr, void *data, uint8_t *mask) {
	Assert(addr < HW_MEM_SIZE, "physical address %x is outside of the physical memory!", addr);

	int i;
	L1cache_addr temp;
	temp.addr = addr & ~BURST_MASK;

	for(i = 0; i < Q_WIDTH1; i++) {
		if(L1cache[temp.r][i].valid == 1 && L1cache[temp.r][i].q == temp.q && L1cache[temp.r][i].f == temp.f){
			memcpy_with_mask(L1cache[temp.r][i].block + temp.w, data, BURST_LEN, mask);
			L2cache_write(addr, data, BURST_LEN, mask);
			write_dram_with_mask(addr, data, BURST_LEN, mask);
			return ;
		}
	}
	L2cache_write(addr, data, BURST_LEN, mask);
	//write_dram_with_mask(addr, data, BURST_LEN, mask);
}

uint32_t L1cache_read(hwaddr_t addr,  size_t len) {
	//Assert(addr < HW_MEM_SIZE, "physical address %x is outside of the physical memory!", addr);
	uint32_t offset = addr & BURST_MASK;
	uint8_t temp[2 * BURST_LEN];

	L1burst_read(addr, temp);
	
	if(offset + len > BURST_LEN) {
		/* data cross the burst boundary */
		L1burst_read(addr + BURST_LEN, temp + BURST_LEN);
	}
	return unalign_rw(temp + offset, 4);
}

void L1cache_read_debug(hwaddr_t addr, size_t len){
	int i;
	L1cache_addr caddr;
	caddr.addr = addr;
	uint32_t temp;
	for(i = 0; i < Q_WIDTH1; i++) {
		if (L1cache[caddr.r][i].q == caddr.q && L1cache[caddr.r][i].f == caddr.f && L1cache[caddr.r][i].valid == 1) {
			if (len + caddr.w <= BLOCK_SIZE1) {
				memcpy(&temp, &L1cache[caddr.r][i].block[caddr.w], len);
				printf("content = %x, cache组号f = %d, 组内块号q = %d\n", temp, caddr.f , caddr.q);
				return ;
			}
		} 
	}
	printf("Can't find in the L1cache！！！\n");
}

void L1cache_write(hwaddr_t addr, size_t len, uint32_t data) {
	uint32_t offset = addr & BURST_MASK;
	uint8_t temp[2 * BURST_LEN];
	uint8_t mask[2 * BURST_LEN];
	memset(mask, 0, 2 * BURST_LEN);

	*(uint32_t *)(temp + offset) = data;
	memset(mask + offset, 1, len);

	L1burst_write(addr, temp, mask);

	if(offset + len > BURST_LEN) {
		/* data cross the burst boundary */
		L1burst_write(addr + BURST_LEN, temp + BURST_LEN, mask + BURST_LEN);
	}
}

void L2cache_read(hwaddr_t addr,  void *data, uint32_t r, uint32_t q) {
	Assert(addr < HW_MEM_SIZE, "physical address %x is outside of the physical memory!", addr);

	int i;
	L2cache_addr temp;
	L2cache_addr dram_addr;
	temp.addr = addr & ~BURST_MASK;
	
	for(i = 0; i < Q_WIDTH2; i++) {
		if(L2cache[temp.r][i].valid == 1 && L2cache[temp.r][i].q == temp.q && L2cache[temp.r][i].f == temp.f) {
			memcpy(data, L2cache[temp.r][i].block + temp.w, BURST_LEN);
			memcpy(L1cache[r][q].block, L2cache[temp.r][i].block, BLOCK_SIZE1);
			return ;
		} 
	}

	for(i = 0; i < Q_WIDTH2; i++) {
		if (L2cache[temp.r][i].valid == 0) {
			L2cache[temp.r][i].q = temp.q;
			L2cache[temp.r][i].f = temp.f;
			L2cache[temp.r][i].valid = 1;
			L2cache[temp.r][i].dirty = 0;
			update_cache(addr, L2cache[temp.r][i].block, BLOCK_SIZE2);
			memcpy(L1cache[r][q].block, L2cache[temp.r][i].block, BLOCK_SIZE1);
			memcpy(data, L2cache[temp.r][i].block + temp.w, BURST_LEN);
		} 
	}

	srand(time(0));
	i = rand()%BLOCK_NUM2;
	if (L2cache[temp.r][i].dirty == 1) {
		dram_addr.q = L2cache[temp.r][i].q;
		dram_addr.r = temp.r;
		dram_addr.f = L2cache[temp.r][i].f;
		dram_addr.w = 0;
		update_dram(dram_addr.addr, L2cache[temp.r][i].block, BLOCK_SIZE2);
	}
	L2cache[temp.r][i].q = temp.q;
	L2cache[temp.r][i].f = temp.f;
	L2cache[temp.r][i].valid = 1;
	L2cache[temp.r][i].dirty = 0;
	update_cache(addr, L2cache[temp.r][i].block, BLOCK_SIZE2);
	memcpy(L1cache[r][q].block, L2cache[temp.r][i].block, BLOCK_SIZE1);
	memcpy(data, L2cache[temp.r][i].block + temp.w, BURST_LEN);
}

void L2cache_write(hwaddr_t addr, void *data, size_t len, uint8_t *mask) {
	Assert(addr < HW_MEM_SIZE, "physical address %x is outside of the physical memory!", addr);

	int i;
	L2cache_addr temp;
	L2cache_addr dram_addr;
	temp.addr = addr & ~(len-1);
	for(i = 0; i < Q_WIDTH2; i++) {
		if(L2cache[temp.r][i].valid == 1 && L2cache[temp.r][i].q == temp.q && L2cache[temp.r][i].f == temp.f){
			memcpy_with_mask(L2cache[temp.r][i].block + temp.w, data, BURST_LEN, mask);
			L2cache[temp.r][i].dirty = 1;
			return ;
		}
	}
	for(i = 0; i < Q_WIDTH2; i++) {
	 	if (L2cache[temp.r][i].valid == 0) {
	 		L2cache[temp.r][i].q = temp.q;
	 		L2cache[temp.r][i].f = temp.f;
	 		L2cache[temp.r][i].valid = 1;
	 		update_cache(addr, L2cache[temp.r][i].block, BLOCK_SIZE2);
	 		memcpy_with_mask(L2cache[temp.r][i].block + temp.w, data, BURST_LEN, mask);
	 		L2cache[temp.r][i].dirty = 1;
	 		return ;
	 	} 
	 }
	 srand(time(0));
	 i = rand()%BLOCK_NUM2;
	 if(L2cache[temp.r][i].dirty == 1) {
	 	dram_addr.q = L2cache[temp.r][i].q;
	 	dram_addr.r = temp.r;
	 	dram_addr.f = L2cache[temp.r][i].f;
		dram_addr.w = 0;
	 	update_dram(dram_addr.addr, L2cache[temp.r][i].block, BLOCK_SIZE2);
	 }
	 L2cache[temp.r][i].q = temp.q;
	 L2cache[temp.r][i].f = temp.f;
	 L2cache[temp.r][i].valid = 1;
	 update_cache(addr, L2cache[temp.r][i].block, BLOCK_SIZE2);
	 memcpy_with_mask(L2cache[temp.r][i].block + temp.w, data, BURST_LEN, mask);
	 L2cache[temp.r][i].dirty = 1;
	 return ;
}
