
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <fuse.h>
#include <errno.h>
#include "disk.h"

#define BLOCK_SIZE 4096
#define BLOCK_NUM 65536
#define FILE_MAX_NUM 32768
#define DISK_SIZE (BLOCK_SIZE*BLOCK_NUM)

#define DIRMODE (S_IFDIR|0755)
#define REGMODE (S_IFREG|0644)

//support function and macro

//macro constant 
#define DEBUG

//bases of bitmaps,inode,free block (block offset):
//inode bitmap base 
#define INODE_BMAP_BASE	1
//inode base 
#define INODE_BASE  4
//free block bitmap base
#define FBLK_BMAP_BASE  2
//free block base
#define FBLK_BASE   590

//inode blks related:
//num of inode per block
#define INODE_NUM_PBLK 56 ///////////////////////////////
//inode size
#define INODE_SIZE 72///////////////////////////
//root directory's inode offset, In most U NIX file systems, the root inode number is 2.
#define ROOT_I  2

//support struct related:
//max_len of the file/directory's name 
#define NAME_MAX_LEN	24
//num of direct, indirect, double indirect pointer
#define DIR_P_NUM	12
#define IND_P_NUM	2
#define	DIND_P_NUM	1
//In directory block,
//name:inode pair per blk
#define DIR_PAIR_PBLK	128
//各项指针的临界大小, 写入时按顺序写入(先direct再indirect最后double indirect)
//12*4096
#define DIRP_MAX_SIZE	49152	
//2*(4096/2)*4096
#define	INDP_MAX_SIZE	16777216
//1*(4096/2)*(4096/2)*4096
#define DINDP_MAX_SIZE	17179869184
//最大文件的大小
#define	FILE_MAX_SIZE	17179869184

//In pointer block,
//"pointer" per blk, 初始化全是-1（即无内容 65536），pointer基于FBLK_BASE，所以不会有65536个
#define PTR_MAX_PBLK	2048
//pointer max count,最多有多少个指针(包括块里的)
#define PTR_MAX_CNT		5000000

//macro function
#define MIN(_min1,_min2)	((_min1) > (_min2) ? (_min2) : (_min1))
#define MAX(_max1,_max2)	((_max1) > (_max2) ? (_max1) : (_max2))

//support struct
struct Inode {
	__mode_t mode;
    __off_t size;
    __time_t atime;
    __time_t mtime;
    __time_t ctime;
    unsigned char pointer_bmap;
    unsigned short dir_pointer[12];
    unsigned short ind_pointer[2];
    unsigned short doub_ind_pointer;
};
//
struct DirPair{
	int inode_num; // == -1 by default (if not a name)
	char name[NAME_MAX_LEN+1];
};



/************** helper function begin **************/

/************** get_inode相关的函数 **************/
/* get_inode 
 * 通过文件路径,获取对应的Inode结构
 * 接口:
 * path: 文件从根目录开始的路径名
 * target: 待修改的inode值, 传入&inode, 修改*target
 * return value:
 * (-1) - disk read error
 * (-2) - filename or path not exist
 * (-3) - root dir not included
 * else return inode_num
 */
int get_inode(const char* path,struct Inode **target) {
	// 不包含根目录，返回-3
	if (strchr(path, '/') == NULL) //////////////////开局就错= =， strchr找不到返回的是NULL，应该是 == NULL才return
		return -3;
	// 只含根目录
	struct Inode* inode_buf;
	if ((*path == '/') && (strlen(path) == 1)){
		int get_inode_iblk_retval = get_inode_iblk(ROOT_I, &inode_buf);
		if (get_inode_iblk_retval == -1)
			return -1;
		*target = inode_buf;
		return ROOT_I;
	}


	size_t path_len = strlen(path);
	// "/xxx/xxx/" or "/xxx/xxx" 都看作 "/xxx/xxx"，统一格式
	if (path[path_len-1] == '/'){
		// path[path_len-1] = '\0'; ///-----------------wrong, path is read only
		path_len--;
	}
    int i;
	// start point to the prev '/', end point to the next '/'
	// filename lies between them
    int start_ptr=0, end_ptr=0;
    char file_name[NAME_MAX_LEN + 1] = ""; 
    // '/'s divide path
    int dir_num = ROOT_I; ////////////////////////////后续称呼inode在inode块内的偏移还是统一为inode_num （这还是你取的名字）

	// get inode offset among folder layers
    for (i = 1; i < path_len; i++){
        if (path[i] == '/'){
            // get file name between two '/'
			end_ptr = i;
			// get file name in dir
			strncpy(file_name, path + start_ptr + 1, end_ptr - start_ptr - 1); ////////////这样拷贝的字符串没有'\0'结尾！！，后续使用在strcmp中会出错
			file_name[end_ptr - start_ptr - 1] = '\0';
			// get the inode num of next layer folder
			dir_num = get_inode_idir(file_name, dir_num);
			// something go wrong, then return -1 or -2
			if (dir_num < 0)
				return dir_num;
			start_ptr = end_ptr; 
        }
    }
	// get inode num from last layer folder  /ljtnb/?
	strncpy(file_name, path + start_ptr + 1, path_len -1 -start_ptr); ////////////////////和上面问题一样，末尾得补'\0'
	file_name[path_len - start_ptr - 1] = '\0';
	int file_num = get_inode_idir(file_name, dir_num); ////////////////////wrong,如果是文件夹，那么file_name="",是这里直接返回的file_offset就是负数了，应该在此处判断区分
	////////////////////////////////////////////////////////////////////////////否则，target无法得到正确值。这几句对文件夹的处理都有点问题，应该直接判断最后一个字符是不是'/'
	//这一部分修改了，如果是"/xxx/xxx/"的形式，则只看"/xxx/xxx"
    if (get_inode_iblk(file_num, &inode_buf) == -1)
		return -1;
	*target = inode_buf;
	return file_num;
}

/* get_inode_idir - get file's inode_num in the directory
 * 在目录中通过文件名获取inode_num
 * 接口:
 * filename: 文件名(不包含路径,不是路径名),(可以是目录)
 * dir_num: 查询目录的inode_num(编号)
 * 返回值:
 * 读取block出错,返回-1
 * 文件未找到,返回-2
 * 其余情况,返回该文件对应的inode_num(编号)
 */
int get_inode_idir(const char* filename, int dir_num){
    struct Inode *dir_inode;
    // get dir inode (struct)
    int flag = get_inode_iblk(dir_num, &dir_inode);
    if (flag == -1)
        return -1;

    // get dir inode pointer number
    int dir_ptr_num = (dir_inode->pointer_bmap & 0xf);
    int ind_ptr_num = (dir_inode->pointer_bmap & 0x30) >> 4;
    int dual_ind_ptr_num = (dir_inode->pointer_bmap & 0x40) >> 6;

    int i, j, k;
    char buf[BLOCK_SIZE + 1];
    struct DirPair* dir_pair;
    // read dir and compare file name to each dir_pair
    for (i = 0; i < dir_ptr_num; i++){
        // ptr used or not, if not, continue
        if (dir_inode->dir_pointer[i] == (unsigned short)(-1)){
            i--;
            continue;
        }
		if (disk_read((unsigned short)FBLK_BASE + dir_inode->dir_pointer[i], buf)) //////////warning: 没有检测回来的值是否是0/1
			return -1;
        dir_pair = (struct DirPair *)buf;
        for (j = 0; j < DIR_PAIR_PBLK; j++, dir_pair++){
            // dir inode num initialize to -1
            // indicates the end of dir
            if (dir_pair->inode_num == -1)
                continue;
            // if names match, return inode num
            if (strcmp(dir_pair->name, filename) == 0){
                return dir_pair->inode_num;
            }
        }
    }
    // read indirect data blocks and compare
    for (i = 0; i < ind_ptr_num; i++){
        if (dir_inode->ind_pointer[i] == (unsigned short)(-1)){
            i--;
            continue;
        }
        char ptr_buf[BLOCK_SIZE];
        if (disk_read((unsigned short)FBLK_BASE + dir_inode->ind_pointer[i], ptr_buf)) //////////warning: 没有检测回来的值是否是0/1
			return -1;
		// pointer number in the indirect pointer block
        // unsigned int ptr_num = (unsigned int)((short)ptr_buf);
        for (j = 0; j < PTR_MAX_PBLK; j++){ /////////////////wrong, DIR_PAIR_PBLK should be PTR_MAX_PBLK
			unsigned short *short_ptr = (unsigned short *)ptr_buf;
            if (*(ptr_buf + j) == (unsigned short)(-1)){ //////////////wrong, should be *(short_ptr+k)
                continue;
            }
            if (disk_read((unsigned short)FBLK_BASE + *(short_ptr + j), buf)) //////////warning: 没有检测回来的值是否是0/1
				return -1;
			dir_pair = (struct DirPair *)buf;
            for (k = 0; k < DIR_PAIR_PBLK; k++, dir_pair++){
                // if names match, return inode num
                if (dir_pair->inode_num == -1)
                    continue;
                if (strcmp(dir_pair->name, filename) == 0){
                    return dir_pair->inode_num;
                }
            }
        }
    }
    // read double indirect data blocks and compare
    if(dual_ind_ptr_num == 1){ /////////////wrong dual_ptr_num should be dual_ind_ptr_num 而且这只有0/1没必要
        char dptr_buf[BLOCK_SIZE + 1];
        if (dir_inode->doub_ind_pointer == (unsigned short)(-1)){ ////////////wrong,double indirect ptr就只有一个，这个不是数组，错了
			return -2;
		}

        if (disk_read((unsigned short)FBLK_BASE + dir_inode->doub_ind_pointer , dptr_buf) )////////// wrong,doub_pointer should be doub_ind_pointer
			return -1;

		// pointer number in the double indirect pointer block
        // unsigned int dptr_num = (unsigned int)((short)dptr_buf);
        
        for (i = 0; i < PTR_MAX_PBLK; i++){ //////////wrong, DIR_PAIR_PBLK should be PTR_MAX_PBLK
			unsigned short *dshort_ptr = (unsigned short *)dptr_buf; 
            if (*(dshort_ptr + i) == (unsigned short)(-1)){
                continue;
            }
            char ptr_buf[BLOCK_SIZE + 1];
            if (disk_read(((unsigned short)FBLK_BASE + *(dshort_ptr + i)), ptr_buf)) //////////warning: 没有检测回来的值是否是0/1
				return -1;
			// unsigned int ptr_num = (unsigned int)((short)ptr_buf);
            // int l; //吐槽一句，j都没用就用k，就用l。顺序有点问题
            for (j = 0; j < PTR_MAX_PBLK; j++){ /////////wrong ptr_num不存在，应该是PTR_MAX_PBLK
				unsigned short *short_ptr = (unsigned short *)ptr_buf;
                if (*(short_ptr + j) == (unsigned short)(-1)){
                    continue;
                }
                if (disk_read((unsigned short)FBLK_BASE + *(short_ptr + j), buf)) /////////warning: 没有检测回来的值是否是0/1
					return -1;
				dir_pair = (struct DirPair *)buf;
                for (k = 0; k < DIR_PAIR_PBLK; k++, dir_pair++){ 
                    // if names match, return inode num
                    if (dir_pair->inode_num == -1)
                        continue;
                    if (strcmp(dir_pair->name, filename) == 0){
                        return dir_pair->inode_num;
                    }
                }
            }
        }
    }

    return -2;    
}

/* get_inode_iblk - get Inode(struct) by inode_num in the inode_blk
 * 通过inode_num 得到相应的inode结构体数据 (在inode_blk中存储的)
 * 接口:
 * inode_num: inode的编号
 * inode: 实际上是指针传参, 传进来&inode, 返回的结构即对指向该inode的指针(解引用)的值做的修改
 * 返回值:
 * 出错,读取块出错,返回-1
 * malloc出错,返回-2
 * 其他情况,返回0
 */
int get_inode_iblk(int inode_num, struct Inode** inode){
    // calculate block id and offset within a block where inode locates
    int blk_id = INODE_BASE + inode_num/INODE_NUM_PBLK;
    int blk_offset = INODE_SIZE * (inode_num % INODE_NUM_PBLK);

    char *buf = malloc(BLOCK_SIZE+1);
	if(NULL == buf) {
		printf("Get_inode_iblk: malloc error\n");
		return -1;
	}
    // get block by id and offset, write to buf
    int flag = disk_read(blk_id, buf);
    if (flag != 1){ //这里也有问题，他是正常返回0，错误返回1
		// $$
		struct Inode *inode_buf = (struct Inode*)(buf + blk_offset);
        // *(inode) = *(inode_buf); //-----------------wrong
		*inode = inode_buf;
        return 0;
    }
    else
        return -1;
}

int free_inode_blk_buffer(int inode_num,struct Inode** inode) {
	int inode_offset_in_blk = inode_num % INODE_NUM_PBLK;
	free(*inode - inode_offset_in_blk);
	*inode = NULL;
}

/************** 对bitmap操作的函数 **************/
/* bitmap_opt - revise bitmap_block
 * bmap_num - block number of bitmap
 * mode - 0 for freeing, 1 for allocating
 * num - inode num of data block num
 * error return -1, else 0
 */
int bitmap_opt(int mode, int num, int bmap_num){
	// read in inode bitmap block
	char buf[BLOCK_SIZE + 1] = "";
	if (disk_read(bmap_num, buf)){
		return -1;
	}

	int byte_id = num / 8;
	int bit_id  = num % 8;
	int opt = mode & 0x1;
	char bmap_stat = (buf[byte_id] >> bit_id) & 0x1;
	char byte_stat = (char)opt;
	
	
	// this means trying to free a free inode 
	// or allocate a allocated inode
	if ((byte_stat^bmap_stat) == 0){
		if (byte_stat == '\1'){// wrong on allocate a allocated struct
			return -1;
		}
		////////free a free inode is not ok 也当错误情况处理吧，返回-2之类的//ok
		else{
			return -2;
		}
	}
	
	// old_stat opt -> new_stat ; the better to change the opt
	// 0 1 -> 1 ; 1 0 -> 0; 
	// 0^(1|1) -> 1 ; 1^(1|0) -> 0; thus 无论free还是alloc都^1,free掉free和alloc给alloced都当错误处理
	// change bit map
	byte_stat = (buf[byte_id]) ^ ((0x1) << bit_id);/////
	buf[byte_id] = byte_stat;
	// write inode bitmap block
	if (disk_write(bmap_num, buf)){
		return -1;
	}
	return 0;
}

/* imap_opt - revise inode bitmap block 
 * 修改inode_bitmap中inode_num对应的bit
 * mode - 0 for freeing, 1 for allocating
 * num - inode num of data block num
 * error return -1 or -2, else 0
 */
int imap_opt(int mode, int inode_num){
	// read in inode bitmap block
	char buf[BLOCK_SIZE + 1] = "";
	if (disk_read(INODE_BMAP_BASE, buf)){
		return -1;
	}

	int byte_id = inode_num / 8;
	int bit_id  = inode_num % 8;
	int opt = mode & 0x1;
	char bmap_stat = (buf[byte_id] >> bit_id) & 0x1;
	char byte_stat = (char)opt;

	// this means trying to free a free inode 
	// or allocate a allocated inode
	if ((byte_stat^bmap_stat) == 0){
		if (byte_stat == '\1'){// wrong on allocate a allocated inode
			return -1;
		}
		else{
			return -2;
		}
		////////free a free inode is not ok 也当错误情况处理吧，返回-2之类的//ok
	}
	// old_stat opt -> new_stat ; the better to change the opt
	// 0 1 -> 1 ; 1 0 -> 0; 
	// 0^(1|1) -> 1 ; 1^(1|0) -> 0; thus 无论free还是alloc都^1,free掉free和alloc给alloced都当错误处理
	// change bit map
	byte_stat = (buf[byte_id]) ^ ((0x1) << bit_id);/////
	buf[byte_id] = byte_stat;
	// write inode bitmap block
	if (disk_write(INODE_BMAP_BASE, buf)){
		return -1;
	}
	return 0;
}


/* bmap_cnt - count free bit numbers in bitmap
 * 数bitmap中未分配块的个数, 即数'0'(二进制bit)
 * return free numbers if nothing wrong, else -1
 */
int bmap_cnt(int bmap_blk_num){
	char buf[BLOCK_SIZE + 1];
	// read in bmap block
	if (disk_read(bmap_blk_num, buf)){
		printf("error: disk read\n");
		return -1;
	}
	int counter = 0;
	char mask = 0x11;
	int helpCnt = 0;
	// for each byte, count its bit and sum up
	for (int i = 0; i < BLOCK_SIZE; i++){
		helpCnt = (int)(buf[i] & mask);
		helpCnt += (int)((buf[i]>>1) & mask);
		helpCnt += (int)((buf[i]>>2) & mask);
		helpCnt += (int)((buf[i]>>3) & mask);
		helpCnt = (helpCnt + (helpCnt >> 4)) & 0xF;
		counter += (int)helpCnt;
	}
	return ((int) BLOCK_SIZE) * 8 - counter;
}

/************** 分配块相关的操作 **************/
//WWWWWWWWWWWWWWWWWWWWWWWWWWWWW 该名称有误导性,建议修改为 get_fblk_sum or get_fblk_cnt
/* get_fblk_num - get free blocks count 
 * 注意是返回当前剩余的free block数
 * return free blocks number if no errors exist, else return -1
 */
int get_fblk_num(){
	int num1 = bmap_cnt(FBLK_BMAP_BASE);
	int num2 = bmap_cnt(FBLK_BMAP_BASE + 1);
	return (-1 == num1) || (-1 == num2) ? -1 : num1+num2;
	// return (num1|num2)== -1 ? -1 : num1+num2; //EEEEEEEEEEEEEEEEEEEEEEEEEE: 这个未必是-1 == num1 || -1 == num2的意思,比如4bit,1010,0101,也会有错误.
	//EEEEEEEEEEEEEEEEEEEEEEEEE: 修改意见, (int)(num1|num2) < 0,这样应该可以. 或者语义清晰点 (-1 == num1) || (-1 == num2)
}

// fblk_list 为 unsigned short的list更好
/* find_fblk - find free blocks using first fit algorithm
 * 会改block bitmap
 * num: request free blocks number
 * fblk_list: used to store free block numbers
 * return (free blocks num - num) if no errors exist
 * return -1 if free blocks num less than num
 * return -3 if num == 0 or fblk_list == NULL
 * else return -2 
 */
int find_fblk(int num, unsigned short *fblk_list){
	// 申请0个或fblk_list为空指针，返回-3
	if ((0 == num) || (NULL == fblk_list)){
		return -3;
	}

	// 统计空闲块数量
	int fblk_num = get_fblk_num();
	if (fblk_num == -1){
		// 统计bmap失败的情况
		printf("error: get freeblock num fail\n");
		return -2;
	}
	//note:或者<=？我们的文件系统会被占满，还是要求预留一些空间？
	else if(fblk_num < num){ 
		// free blocks不足的情况
		printf("no enough blocks: fblk %d - ask for %d\n", fblk_num, num);
		return -1;
	}
	else{
		fblk_num -= num;
	}

	char buf[BLOCK_SIZE + 1];
	// 读取第一个freeblockk bitmap
	if (disk_read(FBLK_BMAP_BASE, buf)){
		printf("error: disk read %d\n", FBLK_BMAP_BASE);
		return -2;
	}
	// 按位检查
	int blk_id = 0;
	char check_bit = 0x1;
	for (int i = 0; i < BLOCK_SIZE; i++){
		for (int j = 0; j < 8; j++, blk_id++){
			// 未被占用则赋值给fblk_list

			if (0 == ((buf[i]>>j) & 0x1)){
				num--;
				*fblk_list = (unsigned short)blk_id; 
				fblk_list++;
			}
			else{
				continue;
			}
			int _flag;
			if ((_flag = bitmap_opt(1, blk_id, FBLK_BMAP_BASE)) < 0){
				printf("Find_fblk: error: bit map opt\n");
				return -1;
			}
			// 分配完成则返回
			if (num == 0){
				return fblk_num;
			}
			
		}
	}
	
	// 读取第二个freeblockk bitmap
	if (disk_read(FBLK_BMAP_BASE+1 , buf)){
		printf("error: disk read %d\n", FBLK_BMAP_BASE);
		return -2;
	}
	// 按位检查
	for (int i = 0; i < BLOCK_SIZE; i++){
		for (int j = 0; j < 8; j++, blk_id++){
			// 未被占用则赋值给fblk_list
			if (0 == ((buf[i]>>j) & 0x1)){
				num--;
				(*fblk_list++) = (unsigned short)blk_id; 
			}
			else{
				continue;
			}
			int _flag;
			if ((_flag = bitmap_opt(1, blk_id%FBLK_BMAP_BASE, FBLK_BMAP_BASE+1)) < 0){
				printf("Find_fblk: error: bit map opt\n");
				return -1;
			}
			// 分配完成则返回
			if (num == 0){
				return fblk_num;
			}
			
		}
	}

	return fblk_num;

}

/* find_finode - find free inode
 * 找到一个空闲的inode, 将其初始化后, 返回其inode_num, inode_bmap已经设置了,等于分配
 * 返回值:
 * -1 : 错误
 * inode_num :正常 (>= 0)
 */
int find_finode() {
	int finode_cnt = bmap_cnt(INODE_BMAP_BASE);
	if(finode_cnt <= 0) {
		printf("Find_finode: no free inode\n");
		return -1;
	}
	
	char inode_bmap[BLOCK_SIZE+1];
	if(disk_read(INODE_BMAP_BASE,inode_bmap)) {
		printf("Find_finode: disk read error\n");
		return -1;
	}
	int inode_num = 0, break_flag = 0;
	for (int i = 0; i < BLOCK_SIZE && !break_flag; i++){
		for (int j = 0; j < 8 && !break_flag ; j++){
			// 未被占用则赋值给fblk_list
			if (0 == ((inode_bmap[i]>>j) & 0x1)){
				inode_bmap[i] |= (1 << j);
				break_flag = 1;
				break;
			}
			inode_num++;
		}
	}
	if(disk_write(INODE_BMAP_BASE,inode_bmap)) {
		printf("Find_finode: disk write error\n");
		return -1;
	}
	return inode_num;
}

/* inode_ptr_add -  allocate block pointer in the inode
 * ptr_list: ptr needed to be removed or added from inode
 * return -1 if errors exist, else 0
 */
// 修改：每个文件按照inodes里面ptr的顺序进行写
int inode_ptr_add(int inode_num, int ptr_num, unsigned short *ptr_list){
	struct Inode* inode;
	if (get_inode_iblk(inode_num, &inode)){
		printf("error: get inode %d\n", inode_num);
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}
	// 空指针或请求数量为0, 直接退出
	if ((NULL == ptr_list) || (ptr_num == 0)){
		free_inode_blk_buffer(inode_num,&inode);
		return 0;
	}

	// __off_t file_size = inode->size;
	int ablk_cnt = allocated_blk_cnt(inode);
	int start_ptr_id = 0;

	// 计算空指针开始的块号所处的inode指针号
	if (ablk_cnt < 12){
		start_ptr_id = ablk_cnt;
	}
	else if(ablk_cnt >= 12 && ablk_cnt < 12 + 2 * (int)PTR_MAX_PBLK){
		start_ptr_id = 12 + (ablk_cnt - 12)/(int)PTR_MAX_PBLK;
	}
	else{
		start_ptr_id = 15;
	}

	int inode_blk_id = (int)INODE_BASE + inode_num/(int)INODE_NUM_PBLK;
	if (start_ptr_id < 12){
		for (int i = start_ptr_id; i < 12 && ptr_num != 0; i++){
			inode->dir_pointer[i] = *ptr_list;
			ptr_num--;
			ptr_list++;
		}
	}
	if (start_ptr_id < 14 && ptr_num != 0){
		for (int i = start_ptr_id - 12 && ptr_num != 0; i < 2; i++){
			if (inode->ind_pointer[i] == (unsigned short )(-1)){	//	不存在则分配
				int fblk_id;
				if (find_fblk(1, &fblk_id) < 0){
					printf("error: find fblk\n");
					return -1;
				}
				inode->ind_pointer[i] = fblk_id;
			}
			char buf[BLOCK_SIZE];
			if (disk_read(inode->ind_pointer[i], buf)){
				printf("error: disk read\n");
				return -1;
			}
			unsigned short *ptr_buf = (unsigned short *)buf;
			for (int j = 0; j < PTR_MAX_PBLK & ptr_num != 0; j++ ){
				*ptr_buf = *ptr_list;
				ptr_list++;
				ptr_buf++;
			}
			if (disk_write(inode->ind_pointer[i], buf)){
				printf("error: disk write\n");
				return -1;
			}
		}

	}
	if (start_ptr_id <= 15 && ptr_num != 0){
		if (inode->doub_ind_pointer == (unsigned short)(-1)){
			int fblk_id;
			if (find_fblk(1, &fblk_id) < 0){
				printf("error: find fblk\n");
				return -1;
			}
			inode->doub_ind_pointer = fblk_id;
		}
		char ind_buf[BLOCK_SIZE];
		if (disk_read(inode->doub_ind_pointer, ind_buf)){
			printf("error: disk read\n");
			return -1;
		}
		unsigned short* ind_ptr = (unsigned short *)ind_buf;
		for (int i = 0; i < PTR_MAX_PBLK && ptr_num!=0; i++){
			if (*ind_ptr == (unsigned short )(-1)){	//	不存在则分配
				int fblk_id;
				if (find_fblk(1, &fblk_id) < 0){
					printf("error: find fblk\n");
					return -1;
				}
				*ind_ptr = fblk_id;
			}
			char buf[BLOCK_SIZE];
			if (disk_read(*ind_ptr, buf)){
				printf("error: disk read\n");
				return -1;
			}
			unsigned short* dir_ptr = (unsigned short *)buf;
			for (int j = 0; j < PTR_MAX_PBLK && ptr_num!=0; j++){
				*dir_ptr = *ptr_list;
				dir_ptr++;
				ptr_list++;
			}
			if (disk_write(*ind_ptr, buf)){
				printf("error: disk write\n");
				return -1;
			}
			ind_ptr++;
		}
		if (disk_write(inode->doub_ind_pointer, ind_buf)){
			printf("error: disk write\n");
			return -1;
		}

	}
	int inode_blk_offset = inode_num%(int)INODE_NUM_PBLK;
	if (disk_write(inode_blk_id, inode-inode_blk_offset)){
		printf("error: disk write\n");
		return -1;
	}
	free_inode_blk_buffer(inode_num,&inode);
	return ptr_num;	
}

/* inode_ptr_del - free block pointer in the inode
 * ptr_type: 0 - dir ptr, 1 - ind ptr, 2 - doub ind ptr
 * start_ptr_num: 第一个要被删除的指针号，0 base
 * return -1 if errors exist, else 0
 */
int indoe_ptr_del(int inode_num, int start_ptr_num){
	struct Inode* inode;
	if (get_inode_iblk(inode_num, &inode)){
		printf("error: get inode\n");
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}
	if (start_ptr_num < 0){
		printf("error: del ptr of %d\n", inode_num);
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}
	
	unsigned short *inode_ptr_list[3] = {inode->dir_pointer, inode->ind_pointer, &inode->doub_ind_pointer};
	// int inode_ptr_del_flag = 0;
	if (start_ptr_num < 12){
		inode_ptr_list[0] = inode->dir_pointer+start_ptr_num;
	}
	else if(start_ptr_num >= 12 && start_ptr_num < 12 + 2 * (int)PTR_MAX_PBLK){
		inode_ptr_list[0] = NULL;
		inode_ptr_list[1] = inode->ind_pointer+(start_ptr_num - 12)/(int)PTR_MAX_PBLK;
	}
	else{
		inode_ptr_list[0] = NULL;
		inode_ptr_list[1] = NULL;
	}


	if (inode_ptr_list[0] != NULL){

		// 指针全置为-1
		for (int i = start_ptr_num; i < 12; i++){
			if (*inode_ptr_list[0] == (unsigned short)(-1)){
				break;
			}
			int bmap_blk_id = (int)(*inode_ptr_list[0])/((int)BLOCK_SIZE * 8) + FBLK_BMAP_BASE;
			int bmap_offset = (int)(*inode_ptr_list[0])%((int)BLOCK_SIZE * 8);
			// 将指针在bitmap中抹去
			if (bitmap_opt(0, bmap_offset, bmap_blk_id)){
				printf("error: bitmap opt\n");
				free_inode_blk_buffer(inode_num,&inode);
				return -1;
			}
			*inode_ptr_list[0] = (unsigned short)(-1);
			inode_ptr_list[0]++;
		}
		start_ptr_num += 12;
	}


	if (inode_ptr_list[1] != NULL){
		int start_ind_blk = (start_ptr_num - 12)/(int)PTR_MAX_PBLK;
		int start_ind_blk_line = (start_ptr_num - 12)%(int)PTR_MAX_PBLK;
		for (int i = start_ind_blk; i < 2; i++){
			char ind_ptr_buf[BLOCK_SIZE + 1];
			if (*inode_ptr_list[1] == (unsigned short)(-1)){
				break;
			}
			if (disk_read(*inode_ptr_list[1], ind_ptr_buf)){
				printf("error:disk read\n");
				free_inode_blk_buffer(inode_num,&inode);
				return -1;
			}

			// ind ptr指向的ptr blk
			unsigned short* dir_ptr = (unsigned short *)ind_ptr_buf;
			for (int j = 0; j < (int)PTR_MAX_PBLK; j++){
				int ptr_num = 12 + i * (int)PTR_MAX_PBLK + j;
				if (ptr_num < start_ptr_num) continue;
				if (*dir_ptr == (unsigned short)(-1)){
					break;
				}
				int bmap_blk_id = (int)(*dir_ptr)/((int)BLOCK_SIZE * 8) + FBLK_BMAP_BASE;
				int bmap_offset = (int)(*dir_ptr)%((int)BLOCK_SIZE * 8);
				// 将ptr在bitmap中置0
				if (bitmap_opt(0, bmap_offset, bmap_blk_id)){
					printf("error: bitmap opt\n");
					free_inode_blk_buffer(inode_num,&inode);
					return -1;
				}
				// 更新ind ptr blk
				*dir_ptr = (unsigned short)(-1);
				dir_ptr++;
			}
			
			if (disk_write(*inode_ptr_list[1], ind_ptr_buf)){
				printf("error: disk write\n");
				free_inode_blk_buffer(inode_num,&inode);
				return -1;
			}

			
			// 将Ind ptr在bitmap中置0
			int bmap_blk_id = (int)(*inode_ptr_list[1])/((int)BLOCK_SIZE * 8) + FBLK_BMAP_BASE;
			int bmap_offset = (int)(*inode_ptr_list[1])%((int)BLOCK_SIZE * 8);
			if (bitmap_opt(0, bmap_offset, bmap_blk_id)){
				printf("error: bitmap opt\n");
				free_inode_blk_buffer(inode_num,&inode);
				return -1;
			}
			// 更新inode中的ind ptr
			*inode_ptr_list[1] = (unsigned short )(-1);
			inode_ptr_list[1]++;
			start_ptr_num += (int)PTR_MAX_PBLK;
		}

	}
	if (inode_ptr_list[2] != NULL && *inode_ptr_list[2] != (unsigned short )(-1)){		
		int start_ind_blk = (start_ptr_num - 12 - 2 * (int)PTR_MAX_PBLK)/(int)PTR_MAX_PBLK;
		int start_ind_blk_line = (start_ptr_num - 12 - 2 * (int)PTR_MAX_PBLK)%(int)PTR_MAX_PBLK;

		// 读取doub ptr指向的ind ptr blk
		char ind_ptr_buf[BLOCK_SIZE + 1];
		if (disk_read(*inode_ptr_list[2], ind_ptr_buf)){
			printf("error:disk read\n");
			free_inode_blk_buffer(inode_num,&inode);
			return -1;
		}
		// 这个循环对doub ptr指向的每个ind ptr指向的块进行更新
		unsigned short *ind_ptr = (unsigned short *)ind_ptr_buf;
		for (int i = start_ind_blk; i < (int)PTR_MAX_PBLK; i++){
			if (*ind_ptr == (unsigned short)(-1)){ // ind 为空，则到头了
				break;
			}
			char ptr_buf[BLOCK_SIZE + 1];
			if (disk_read((int)*ind_ptr, ptr_buf)){	// 读取ind ptr blk存储的dir ptr
				printf("erorr: disk read\n");
				free_inode_blk_buffer(inode_num,&inode);
				return -1;
			}
			unsigned short* dir_ptr = (unsigned short*)ptr_buf;	// 对每个dir ptr，将其赋值为-1，并在bmap中free
			for (int j = 0; j < (int)PTR_MAX_PBLK; j++){
				int ptr_num = 12 + 2 * (int)PTR_MAX_PBLK + (i - 1)*((int)PTR_MAX_PBLK) + j;
				if (ptr_num < start_ptr_num) continue;
				if (*dir_ptr == (unsigned short)(-1)){
					break;
				}
				*dir_ptr = (unsigned short)(-1);
				int bmap_blk_id = (int)(*dir_ptr)/((int)BLOCK_SIZE*8) + FBLK_BMAP_BASE;
				int bmap_offset = (int)(*dir_ptr)%((int)BLOCK_SIZE*8);
				if (bitmap_opt(0, bmap_offset, bmap_blk_id)){
					printf("error: bitmap opt\n");
					free_inode_blk_buffer(inode_num,&inode);
					return -1;
				}
				dir_ptr++;
			}
			int bmap_blk_id = (int)(*ind_ptr)/((int)BLOCK_SIZE*8)+ FBLK_BMAP_BASE; // 更新bitmap，free这个ind ptr指向的块
			int bmap_offset = (int)(*ind_ptr)%((int)BLOCK_SIZE*8);
			if (bitmap_opt(0, bmap_blk_id, bmap_offset)){
				printf("error: bitmap opt\n");
				free_inode_blk_buffer(inode_num,&inode);
				return -1;
			}
			if (disk_write((int*)ind_ptr, ptr_buf)){
				printf("error: disk write\n");
				free_inode_blk_buffer(inode_num,&inode);
				return -1;
			}
			ind_ptr++;
		}
		if (start_ind_blk == 0 && start_ind_blk_line == 0){
			// 说明doub ind ptr也被删除，更新 inode
			int bmap_blk_id = (int)(inode->doub_ind_pointer)/((int)BLOCK_SIZE * 8) + FBLK_BMAP_BASE;
			int bmap_offset = (int)(inode->doub_ind_pointer)%((int)BLOCK_SIZE * 8);
			if (bitmap_opt(0, bmap_blk_id,  bmap_offset)){
				printf("error: bitmap opt\n");
				free_inode_blk_buffer(inode_num,&inode);
				return -1;
			}
			inode->doub_ind_pointer = (unsigned short)(-1);
		}

	}
	//更新Inode
	int inode_blk_id = inode_num/(int)INODE_NUM_PBLK + INODE_BASE;
	int inode_blk_offset = inode_num%(int)INODE_NUM_PBLK;
	if (disk_write(inode_blk_id, inode-inode_blk_offset)){
		printf("error: disk write\n");
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}
	free_inode_blk_buffer(inode_num,&inode);
	return 0;
}


/* rm_file_in_parent_dir
 * 
 */

int rm_file_in_parent_dir(const char *child_file_path){
	// "/xxx/xxx/" or "/xxx/xxx" 都看作 /xxx/xxx
	printf("rm file in parent dir: start\n");
	printf("rm_file_in_dir:%s\n", child_file_path);
	//先解析路径,将path分为父目录和文件(夹)
	char path[BLOCK_SIZE];
	memcpy(path, child_file_path, strlen(child_file_path));
	char parent_path[BLOCK_SIZE]; //长度只是为了能装下path
	char name[NAME_MAX_LEN+1]; //该文件的文件名
	strcpy(parent_path,child_file_path);
	size_t len = strlen(child_file_path);
	if(child_file_path[len-1] == '/') {
		parent_path[len-1] = '\0';
		len--;
	}
	char* end_ptr = strrchr(parent_path,'/'); //找到最后一个'/'的位置
	strcpy(name,end_ptr+1); 
	if(end_ptr != parent_path) {
		*end_ptr = '\0'; //该位置置为'\0'
	}
	else{
		*(end_ptr + 1) = '\0';
	}
	//解析完路径,获取父目录的inode
	struct Inode* parent_inode;
	int parent_inode_num;
	parent_inode_num = get_inode(parent_path,&parent_inode);
	if( parent_inode_num < 0) {
		printf("rm_file_in_parent_dir: get inode error\n");
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		return 0;
	}
	//获取父目录的inode
		//修改mtime和ctime
		//在父目录的DirPair块中,添加一项
		//这个函数还是和写入时一样,封装一下
		//判断父目录的块是否需要新分配块,封装一下
		//如果新分配块失败,也返回-ENOSPC

	// 删去表项
	// 下面是获得所有datablock 的ptr list
	// unsigned short ptr_list[PTR_MAX_CNT]; //WWWwWWWWWWWWWWWWWWWWWWW
	unsigned short *ptr_list = malloc(sizeof(unsigned short) * PTR_MAX_CNT);
	unsigned char ptr_bmap = parent_inode->pointer_bmap;
	unsigned int ptr_cnt0 = ptr_bmap & 0xf;
	unsigned int ptr_cnt1 = (ptr_bmap >> 4) & 0x3;
	unsigned int ptr_cnt2 = ptr_bmap >> 6;
	size_t ptr_n = 0;

	for (int i = 0; i < (int)ptr_cnt0; i++, ptr_n ++){
		ptr_list[ptr_n] = parent_inode->dir_pointer[i];
		ptr_list[ptr_n];
	}

	for (int i = 0; i < (int)ptr_cnt1 && (int)ptr_cnt0 == 12; i++){
		char buf[BLOCK_SIZE + 1];
		if (disk_read(parent_inode->ind_pointer[i]+(unsigned short)FBLK_BASE, buf)){
			printf("error: disk read\n");
			free(ptr_list);
			free_inode_blk_buffer(parent_inode_num,&parent_inode);
			return -1;		
		}
		
		unsigned short* dir_ptr = (unsigned short *)buf;
		for (int j = 0; j < PTR_MAX_PBLK; j++, ptr_n++){
			if (*dir_ptr == (unsigned short *)(-1)){
				break;
			}
			ptr_list[ptr_n] = *dir_ptr;
			dir_ptr++;
		}
	}

	if (ptr_cnt2 != 0){
		char ind_buf[BLOCK_SIZE + 1];
		if (disk_read(parent_inode->doub_ind_pointer+(unsigned short)FBLK_BASE, ind_buf)){
			printf("error: disk read\n");
			free(ptr_list);
			free_inode_blk_buffer(parent_inode_num,&parent_inode);
			return -1;		
		}
		unsigned short *ind_ptr = (unsigned short*)ind_buf;
		for (int i = 0; i < PTR_MAX_PBLK; i++){
			if (*ind_ptr == (unsigned short)(-1)){
				break;
			}
			char buf[BLOCK_SIZE + 1];
			if (disk_read((int)(*ind_ptr)+(unsigned short)FBLK_BASE, buf)){
				printf("error: disk read\n");
				free(ptr_list);
				free_inode_blk_buffer(parent_inode_num,&parent_inode);
				return -1;
			}
			unsigned short *dir_ptr = (unsigned short *)buf;
			for (int j = 0; j < PTR_MAX_PBLK; j++, ptr_n++){
				if (*dir_ptr == (unsigned short)(-1)){
					break;
				}
				ptr_list[ptr_n] = *dir_ptr;
				dir_ptr ++;
			}
			ind_ptr++;
		}
	}
	struct DirPair *dir_pair_ptr;
	int del_dp_blk_id = -1;
	int del_dp_blk_offset;
	for (int i = 0; i < ptr_n && del_dp_blk_id == -1; i++){ // 在每个blk中寻找表项
		char buf[BLOCK_SIZE + 1];
		if (disk_read(ptr_list[i]+(unsigned short)FBLK_BASE, buf)){	// 读出blk
			printf("error: disk read\n");
			free_inode_blk_buffer(parent_inode_num,&parent_inode);
			return -1;
		}
		dir_pair_ptr = (struct DirPair*)buf;
		for (int j = 0; j < DIR_PAIR_PBLK; j++){
			if (dir_pair_ptr->inode_num == -1){	// 读完了
				break;
			}
			if (strcmp(name, dir_pair_ptr->name) == 0){	// 匹配成功 // 找到对应的blk num和第几个
				del_dp_blk_id = (int)ptr_list[i];
				del_dp_blk_offset = j;
				printf("rm file in parent dir: %s %d %d\n", dir_pair_ptr->name, del_dp_blk_offset,del_dp_blk_id);
				break;
			}
			dir_pair_ptr++;
		}
	}

	// 更新blk
	char buf[BLOCK_SIZE + 1];
	if (disk_read(ptr_list[ptr_n-1]+(unsigned short)FBLK_BASE, buf)){		// 读出最后一块
		printf("error: disk read\n");
		free(ptr_list);
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		return -1;
	}
	dir_pair_ptr = (struct DirPair *)buf;
	for (int i = 0; i < DIR_PAIR_PBLK; i++){
		if (dir_pair_ptr->inode_num == -1){
			dir_pair_ptr--;
			//将匹配成功的dir pair替换
			char dp_buf[BLOCK_SIZE + 1];
			if (disk_read(del_dp_blk_id+(unsigned short)FBLK_BASE, dp_buf)){		
				printf("error: disk read\n");
				free(ptr_list);
				free_inode_blk_buffer(parent_inode_num,&parent_inode);
				return -1;
			}
			struct DirPair *dp_ptr = (struct DirPair *)dp_buf;	
			dp_ptr += del_dp_blk_offset;
			memcpy(dp_ptr, dir_pair_ptr, sizeof(dir_pair_ptr));
			if (del_dp_blk_id == ptr_list[ptr_n - 1]){
				dp_ptr  = (struct DirPair*)dp_buf + i-1;
				memset(dp_ptr, -1, sizeof(struct DirPair));
			}
			else{
				memset(dir_pair_ptr, -1, sizeof(struct DirPair));
				if (disk_write(ptr_list[ptr_n-1]+(unsigned short)FBLK_BASE, buf)){
					printf("error: disk write\n");
					free(ptr_list);
					free_inode_blk_buffer(parent_inode_num,&parent_inode);
					return -1;
				}				
			}
			
			if (disk_write(del_dp_blk_id+(unsigned short)FBLK_BASE, dp_buf)){
				printf("error: disk write\n");
				free(ptr_list);
				free_inode_blk_buffer(parent_inode_num,&parent_inode);
				return -1;
			}

			if (i == 1){ // 检查末尾块是否为空，为空则删去
				if (indoe_ptr_del(parent_inode_num, ptr_n)){
					printf("error: inode ptr del\n");
					free(ptr_list);
					free_inode_blk_buffer(parent_inode_num,&parent_inode);
					return -1;
				}
			}
			break;
		}
		dir_pair_ptr++;
	}

	// 更新inode
	time_t cur_time = time(NULL);
	parent_inode->mtime = cur_time;
	parent_inode->ctime = parent_inode->mtime;
	parent_inode->size -= sizeof(struct DirPair);
	int inode_blk_id = parent_inode_num/(int)INODE_NUM_PBLK + (int)INODE_BASE;
	int inode_blk_offset = parent_inode_num %(int)INODE_NUM_PBLK;
	if (disk_write(inode_blk_id, parent_inode - inode_blk_offset)){
		printf("error: disk write\n");
		free(ptr_list);
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		return -1;
	}


	free(ptr_list);
	free_inode_blk_buffer(parent_inode_num,&parent_inode);
	return 0;
}


/* rmfile
 * 	删去path文件（夹）
 * return -1 if errors exist, else 0
 */

int rmfile(const char* path){
	// 删除文件
	// 1 获取Inode
	// 2 删除数据块
	// 3 删除Inode
	// 4 更新父目录
	struct Inode *inode;
	int inode_num;
	inode_num =  get_inode(path, &inode);
	if (inode_num < 0){	// 1 获得Inode和Inode num
		printf("erorr: get inode\n");
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}
	if (indoe_ptr_del(inode_num, 0)){	// 2 rm 一个文件，则先需要删去其所有的指针
		printf("error: inode ptr del\n");
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}
	
	int inode_blk_id = inode_num/(int)INODE_NUM_PBLK + INODE_BASE;	// 3 删去Inode
	int inode_blk_offset = inode_num % (int)INODE_NUM_PBLK;
	memset(inode, -1, sizeof(struct Inode)); //将Inode置为-1
	if (disk_write(inode_blk_id, inode - inode_blk_offset)){ // 更新Inode blk
		printf("error:disk write\n");
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}
	int inode_bmap_id = inode_num/((int)BLOCK_SIZE*8) + INODE_BMAP_BASE;
	int inode_bmap_offset = inode_num % ((int)BLOCK_SIZE*8);
	if (bitmap_opt(0, inode_bmap_offset, inode_bmap_id)){
		printf("error: bitmap opt\n");
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}


	if (rm_file_in_parent_dir(path)){	// 4 更新父母录
		printf("error: rm file in parent dir\n");
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}
	free_inode_blk_buffer(inode_num,&inode);
	return 0;
}



/************** helper function end **************/



//Format the virtual block device in the following function
//error returns -1, else 0
int mkfs(){

	// write super block
	char buf[BLOCK_SIZE+1];

	memset(buf, 0, BLOCK_SIZE);
	struct statvfs *stat = (struct statvfs*)buf;
	stat->f_bsize = (unsigned long)BLOCK_SIZE; ////////////宏改名 BLOCK_MAX_SIZE 或者 TOTAL_BLK_SIZE
	stat->f_blocks = (__fsblkcnt_t)BLOCK_NUM;
	stat->f_bfree = (__fsblkcnt_t)(BLOCK_NUM - FBLK_BASE);
	stat->f_bavail = stat->f_bfree;
	stat->f_files = (__fsblkcnt_t)FILE_MAX_NUM;
	stat->f_ffree = (__fsblkcnt_t)(FILE_MAX_NUM - 1);
	stat->f_favail = stat->f_ffree;
	stat->f_namemax = (unsigned long)NAME_MAX_LEN;
	if (disk_write(0, buf)){
		printf("error: disk write error\n");
		return -1;
	}

	// initialize inode bitmap and datablock bitmap
	memset(buf, 0, BLOCK_SIZE);
	if (disk_write(INODE_BMAP_BASE, buf)){
		printf("error: disk write error\n");
		return -1;
	}
	if (disk_write(FBLK_BMAP_BASE, buf)){
		printf("error: disk write error\n");
		return -1;
	}
	if (disk_write(FBLK_BMAP_BASE+1, buf)){
		printf("error: disk write error\n");
		return -1;
	}
	
	// initialize inode blocks
	memset(buf, -1, BLOCK_SIZE);
	// initialize inode data block
	if (disk_write(FBLK_BASE, buf)){
		return -1;
	}
	for (int i = INODE_BASE; i < FBLK_BASE; i++){
		if (disk_write(i, buf)){
			printf("error: disk write error\n");
			return -1;
		}
	}

	// write root dir information into imap and inode blk
	// write inode bitmap block
	if (bitmap_opt(1, ROOT_I, INODE_BMAP_BASE)){
		printf("error: root imap opt\n");
		return -1;
	}
	
	
	// write inode data block
	struct Inode *inode = ((struct Inode *)buf)+ROOT_I; 
	inode->mode = (__mode_t)DIRMODE;
	inode->size = (__off_t)0;
	inode->atime = time(NULL);
	inode->ctime = inode->atime;
	inode->mtime = inode->atime;
	inode->pointer_bmap = (unsigned char)1;/////测试read，不测试赋值为0
	inode->dir_pointer[0] = 0;/////测试read，不测试时删去, ERROR, should be zero
	if (bitmap_opt(1, 0, FBLK_BMAP_BASE)){
		printf("error: bmap opt\n");
		return -1;
	}

	if (disk_write(INODE_BASE, buf)){
		printf("error: disk write error\n");
		return -1;
	}
	
	printf("Mkfs is called\n");
	return 0;
} 

//Filesystem operations that you need to implement

int fs_statfs (const char *path, struct statvfs *stat){
	(void) path;//没有意义的参数

	stat->f_bsize = (unsigned long)BLOCK_SIZE;
	stat->f_blocks = (__fsblkcnt_t)(BLOCK_NUM);
	stat->f_bfree = (__fsblkcnt_t)(bmap_cnt(FBLK_BMAP_BASE) + bmap_cnt(FBLK_BMAP_BASE +1));
	stat->f_bfree -= (__fsblkcnt_t)(FBLK_BASE);
	stat->f_bavail = stat->f_bfree;
	stat->f_files = (__fsfilcnt_t)(FILE_MAX_NUM);
	stat->f_ffree = (__fsblkcnt_t)(bmap_cnt(INODE_BMAP_BASE));
	stat->f_favail = stat->f_ffree;
	stat->f_namemax = (unsigned long)NAME_MAX_LEN;

	printf("Statfs is called:%s\n",path);
	return 0;
}

int fs_getattr (const char *path, struct stat *attr)
{
    if(NULL == path) {
        printf("Getattr: error: path is NULL, path's \"%s\"\n",path);
        return -ENOENT;
    }

    struct Inode *inode;

	int inode_num = get_inode(path,&inode);

	switch (inode_num) {
		case -1:
			printf("Getattr: error: disk read error, path's \"%s\"\n",path);
			free_inode_blk_buffer(inode_num,&inode);
			return -ENOENT;
			break;
		case -2:
			printf("Getattr: error: path doesn't exist, path's \"%s\"\n",path);
			free_inode_blk_buffer(inode_num,&inode);
			return -ENOENT;
			break;
		case -3:
			printf("Getattr: error: path error, path's \"%s\"\n",path);
			free_inode_blk_buffer(inode_num,&inode);
			return -ENOENT;
			break;
		default:
			break;
	}

	attr->st_mode = inode->mode;
	attr->st_nlink = 1;
	attr->st_uid = getuid();
	attr->st_gid = getgid();
	attr->st_size = inode->size;
	attr->st_atime = inode->atime;
	attr->st_mtime = inode->mtime;
	attr->st_ctime = inode->ctime;

	printf("Getattr is called:%s\n",path);
	free_inode_blk_buffer(inode_num,&inode);
	return 0;
}

int fs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi){
	(void) offset;
	(void) fi;
	// 判断路径是否为空，为空则报错
	if(NULL == path) {
        printf("readdir error: path is NULL, path is \"%s\"\n",path);
        return -ENOENT;
    }
	// 路径正常，则读取path下dir的inode
	// inode的地址写入inode指针中
    struct Inode *dir_inode;
	int inode_num = get_inode(path, &dir_inode);
	switch (inode_num) {
		case -1:
			printf("readdir error: disk read error, path is \"%s\"\n",path);
			free_inode_blk_buffer(inode_num,&dir_inode);
			return -ENOENT;
			break;
		case -2:
			printf("readdir error: path doesn't exist, path is \"%s\"\n",path);
			free_inode_blk_buffer(inode_num,&dir_inode);
			return -ENOENT;
			break;
		case -3:
			printf("readdir error: path error, path is \"%s\"\n",path);
			free_inode_blk_buffer(inode_num,&dir_inode);
			return -ENOENT;
			break;
		default:
			break;
	}
	// errFlag正常时即为dir_inode_num
	int dir_inode_num = inode_num;

	// 判断根据path读取的inode是否为目录
	if (dir_inode->mode == (__mode_t)(REGMODE)){
		printf("readdir error: not a directory:%s (readdir)\n", path);
		free_inode_blk_buffer(inode_num,&dir_inode);
		return -1;
	}
	
	// 读取文件列表
	// get dir inode pointer number
	// 这一段复制之前用过的
    int dir_ptr_num = (dir_inode->pointer_bmap & 0xf);
    int ind_ptr_num = (dir_inode->pointer_bmap & 0x30) >> 4;
    int dual_ind_ptr_num = (dir_inode->pointer_bmap & 0x40) >> 6;
    
	// 复制原来路径名
	// 将"/xxx/xxx"、"/xxx/xxx/"统一形式为"/xxx/xxx/"
	// 便于后面拼接文件所在路径以寻找文件inode
	// been tested
	int path_len = strlen(path);
	char dir_path[256] = "";
	memcpy(dir_path, path, path_len);
	if (path[path_len - 1] != '/'){
		dir_path[path_len] = '/';
		dir_path[path_len + 1] = '\0';
		path_len++;
	}
	else{
		dir_path[path_len] = '\0';
	}

	// 更新目录文件atime/////////////////
	dir_inode->atime = time(NULL);
	if (disk_write(INODE_BASE + dir_inode_num/INODE_NUM_PBLK, dir_inode-(dir_inode_num % INODE_NUM_PBLK))){
		printf("readdir error: disk write\n");
		free_inode_blk_buffer(inode_num,&dir_inode);
		return -1;
	}

    int i, j, k, buf_offset = 0;
	char buf[BLOCK_SIZE+1];
	char file_path[256] = "";
    struct DirPair* dir_pair;

    // read dir and compare file name to each dir_pair
	// 1. direct data block
    for (i = 0; i < dir_ptr_num; i++){
        // printf("readdir:  dir ptr\n");
	    // ptr used or not, if not, continue
        if (dir_inode->dir_pointer[i] == (unsigned short)(-1)){
            // i--;
            break;
        }
		// read in data block
		if (disk_read((unsigned short)FBLK_BASE + dir_inode->dir_pointer[i], buf)) {
			free_inode_blk_buffer(inode_num,&dir_inode);
			return -1;
		}
		// initialize dir_pair ptr to the beginnning of the buffer
        dir_pair = (struct DirPair *)buf;
		// compare file name to dir_pairs in this blockm
        for (j = 0; j < DIR_PAIR_PBLK; j++, dir_pair++){
            // dir inode num initialize to -1
            if (dir_pair->inode_num == (-1)){
			    break;
			}

			// concat for file path
			// been tested
			// memset(file_path, 0, sizeof(file_path)); EEEEEEEEEEEEEEEEEEEEEEE: sizeof(file_path) == sizeof(char*)
			memset(file_path,0,256);
			strcat(file_path, dir_path);
			strcat(file_path, dir_pair->name);
			
			struct stat attr;
			// call fs_getattr to get file stat
            // printf("readdir: %d %d %s %x\n", j, dir_pair->inode_num, dir_pair->name, dir_pair);
            if (fs_getattr(file_path ,&attr)){
				printf("readdir error: fs_getattr\n");
				free_inode_blk_buffer(inode_num,&dir_inode);
				return -1;
			}
			
			// write file stat into buffer
			printf("readdir: file - %s\n", dir_pair->name);
			filler(buffer, dir_pair->name, NULL, 0);

        }
    }


	// 2. read indirect datablock
    for (i = 0; i < ind_ptr_num; i++){
		// if indirect pointer isn't used, continue to next indirect pointer
        // printf("readdir:  indir ptr\n");
        if (dir_inode->ind_pointer[i] == (unsigned short)(-1)){
            i--;
            break;
        }
        char ptr_buf[BLOCK_SIZE + 1];
		// read in indirect pointer block
        if (disk_read((unsigned short)FBLK_BASE + dir_inode->ind_pointer[i], ptr_buf)) {
			free_inode_blk_buffer(inode_num,&dir_inode);
			return -1;
		}

		// pointer number in the indirect pointer block
        // unsigned int ptr_num = (unsigned int)((short)ptr_buf);
		unsigned short *short_ptr = (unsigned short *)ptr_buf;
        for (j = 0; j < PTR_MAX_PBLK; j++){ 
            // if the pointer isn't used, continue to next pointer
			if (*(ptr_buf + j) == (unsigned short)(-1)){
                break;
            }
            
			// read in direct pointer data block
			if (disk_read((unsigned short)FBLK_BASE + *(short_ptr + j), buf)) {
				free_inode_blk_buffer(inode_num,&dir_inode);
				return -1;
			}
			
			dir_pair = (struct DirPair *)buf;
            for (k = 0; k < DIR_PAIR_PBLK; k++, dir_pair++){
                // if the dir_pair inode isn't used, continue to next dir_pair
				if (dir_pair->inode_num == -1)
                    break;
				
				// concat for file path
				// been tested
				memset(file_path,0,256);
				strcat(file_path, dir_path);
				strcat(file_path, dir_pair->name);
				
				struct stat attr;
				// call fs_getattr to get file stat
				if (fs_getattr(file_path ,&attr)){
					printf("readdir error: fs_getattr\n");
					free_inode_blk_buffer(inode_num,&dir_inode);
					return -1;
				}
				
				// write file stat into buffer
				printf("readdir: file - %s\n", dir_pair->name);
				filler(buffer, dir_pair->name, NULL, 0);
            }
        }
    }

	// 3. read double indirect datablock	
	if((dual_ind_ptr_num == 1) && (dir_inode->doub_ind_pointer == (unsigned short)(-1))){
        // printf("readdir:  double indir ptr\n");
        char dptr_buf[BLOCK_SIZE + 1];
		// if double indirect pointer isn't used, continue
		// read in double pointer datablock
        if (disk_read((unsigned short)FBLK_BASE + dir_inode->doub_ind_pointer , dptr_buf)) {
			free_inode_blk_buffer(inode_num,&dir_inode);
			return -1;
		}

		unsigned short *dshort_ptr = (unsigned short *)dptr_buf; 
		// for each indirect pointer, read datablock
        for (i = 0; i < PTR_MAX_PBLK; i++){ 
			// if this indirect pointer isn't used, continue
            if (*(dshort_ptr + i) == (unsigned short)(-1)){
                break;
            }

			// read in indirect pointer datablock
            char ptr_buf[BLOCK_SIZE + 1];
            if (disk_read(((unsigned short)FBLK_BASE + *(dshort_ptr + i)), ptr_buf)) {
				free_inode_blk_buffer(inode_num,&dir_inode);
				return -1;
			}

			// read in direct pointer datablock
			unsigned short *short_ptr = (unsigned short *)ptr_buf;
            for (j = 0; j < PTR_MAX_PBLK; j++){
				// if direct pointer isn't used, continue to next one
                if (*(short_ptr + j) == (unsigned short)(-1)){
                    break;
                }
				// read in datablocks
                if (disk_read((unsigned short)FBLK_BASE + *(short_ptr + j), buf)){ /////////warning: 没有检测回来的值是否是0/1
					free_inode_blk_buffer(inode_num,&dir_inode);
					return -1;
				}	
				dir_pair = (struct DirPair *)buf;
                for (k = 0; k < DIR_PAIR_PBLK; k++, dir_pair++){ 
					// if the dir_pair inode isn't used, continue to next dir_pair
					if (dir_pair->inode_num == -1)
						break;
					
					
					// concat for file path
					// been tested
					memset(file_path,0,256);
					strcat(file_path, dir_path);
					strcat(file_path, dir_pair->name);
					
					struct stat attr;
					// call fs_getattr to get file stat
					if (fs_getattr(file_path ,&attr)){
						printf("readdir error: fs_getattr\n");
						free_inode_blk_buffer(inode_num,&dir_inode);
						return -1;
					}
					
					// write file stat into buffer
					printf("readdir: file - %s\n", dir_pair->name);
					filler(buffer, dir_pair->name, NULL, 0);
                }
            }
        }
    }
	printf("Readdir is called:%s\n", path);
	free_inode_blk_buffer(inode_num,&dir_inode);
	return 0;
}

//WWWWWWWWWWWWWWWWWWWWWWWWW:尚未通过大文件测试(indirect 和 double indirect)
int fs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	//size means the req uired size
	(void) fi; //消除unused
	struct Inode *inode;
	int inode_num = get_inode(path, &inode);
	

	switch (inode_num) {
		case -1:
			printf("read error: disk read error, path's \"%s\"\n",path);
			free_inode_blk_buffer(inode_num,&inode);
			return 0;
			break;
		case -2: 
			printf("read error: path doesn't exist, path's \"%s\"\n",path);
			free_inode_blk_buffer(inode_num,&inode);
			return 0;
			break; 
		case -3:
			printf("read error: disk read error, path's \"%s\"\n",path);
			free_inode_blk_buffer(inode_num,&inode);
			return 0;
			break;
		default:
			break;
	}
	//修改文件的atime
	inode->atime = time(NULL);

	if (inode->mode	== ((__mode_t)(DIRMODE)) ) { ///有需要吗?
		printf("Read: error, path's a directory. Path's \"%s\"\n",path);
		free_inode_blk_buffer(inode_num,&inode);
		return 0;
	}
	if (offset > inode->size) {
		printf("Read: error, offset > real size of file. Path's \"%s\"\n",path);
		free_inode_blk_buffer(inode_num,&inode);
		return 0;
	}


	unsigned char ptr_bmap = inode->pointer_bmap;
	unsigned int ptr_cnt0 = ptr_bmap & 0xf;
	unsigned int ptr_cnt1 = (ptr_bmap >> 4) & 0x3;
	unsigned int ptr_cnt2 = ptr_bmap >> 6;
	size_t nleft = size; //size that left to read
	nleft = MIN(nleft,(inode->size-offset));
	size_t ngot = 0; //size that is atually read
	int is_offset = 1; //通过is_offset来控制起始读取位置,以及读完之后接着读.

	//direct pointer
	if (offset < DIRP_MAX_SIZE) {
		//for example: blk_size = 4, offset = 4, ptr_start_index = 1, blk_start_index = 0 (0base)
		int ptr_start_index = offset / BLOCK_SIZE;
		int blk_start_index = offset % BLOCK_SIZE;
		is_offset = 0;
		//not atual end index, may be +1 or even +2(size = m*blk_size, blk_start_index = 0)
		int ptr_end_index = ptr_start_index + (size / BLOCK_SIZE) + 1;
		ptr_end_index = MIN(ptr_end_index, (DIR_P_NUM-1));
		for(int i = ptr_start_index;i <= ptr_end_index && nleft > 0;i++) {
			if(inode->dir_pointer[i] == ((unsigned short)-1) ) {
				if(nleft > 0) {
					printf("Read: error, 文件写入时不按顺序\n");
					free_inode_blk_buffer(inode_num,&inode);
					return ngot;
				}
				continue;//??????????????? break?
			}
			char src_buffer[BLOCK_SIZE];
			if(disk_read((unsigned short)FBLK_BASE + inode->dir_pointer[i],src_buffer) != 0) {
				printf("Read: error, disk_read error\n");
				free_inode_blk_buffer(inode_num,&inode);
				return ngot;
			}
			
			if(nleft < (BLOCK_SIZE-blk_start_index)) {
				memcpy(buffer+ngot,((char*)&src_buffer)+blk_start_index,nleft);
				ngot += nleft;
				nleft -= nleft;
				blk_start_index = 0;	
				
			}
			else {
				memcpy(buffer+ngot,((char*)&src_buffer)+blk_start_index,BLOCK_SIZE-blk_start_index);
				blk_start_index = 0;
				ngot += (BLOCK_SIZE-blk_start_index);
				nleft -= (BLOCK_SIZE-blk_start_index);
				
			}
		}
	}
	
	//indirect pointer
	if(offset < (DIRP_MAX_SIZE+INDP_MAX_SIZE) && nleft > 0) {
		int ptr0_start_index = 0;
		int ptr1_start_index = 0;
		int blk_start_index = 0;
		if(is_offset) {
			//下面这三个高危错误区,不太确定对不对
			ptr0_start_index = (offset - DIRP_MAX_SIZE) / (INDP_MAX_SIZE / 2);
			ptr1_start_index = (offset - DIRP_MAX_SIZE - (ptr0_start_index * (INDP_MAX_SIZE / 2))) / BLOCK_SIZE; 
			blk_start_index = (offset - DIRP_MAX_SIZE - (ptr0_start_index * (INDP_MAX_SIZE / 2)) - (ptr1_start_index * BLOCK_SIZE));
			is_offset = 0;
		}

		for(int i = ptr0_start_index;i < 2 && nleft > 0;i++) {
			if(inode->ind_pointer[i] ==  ((unsigned short)(-1))) {
				if(nleft > 0) {
					printf("Read: error, 文件写入时不按顺序\n");
					free_inode_blk_buffer(inode_num,&inode);
					return ngot;
				}
				continue;
			}
			char ptr_buffer[BLOCK_SIZE];
			if(disk_read((unsigned short)FBLK_BASE + inode->ind_pointer[i],ptr_buffer) != 0){
				printf("Read: error, disk_read error\n");
				free_inode_blk_buffer(inode_num,&inode);
				return ngot;
			}
			unsigned short* ptr_to_data = (unsigned short*) ptr_buffer;
			ptr_to_data += ptr1_start_index;
			//写入时已经确保了一级指针块中,指针是连续的
			for(int j = ptr1_start_index;j < PTR_MAX_PBLK && nleft > 0;j++,ptr_to_data++) {
				if( *ptr_to_data == ((unsigned short)-1)) {
					if(nleft > 0) {
						printf("Read: error, 文件写入时不按顺序\n");
						free_inode_blk_buffer(inode_num,&inode);
						return ngot;
					}
					break;
				}
				char src_buffer[BLOCK_SIZE];
				if(disk_read((unsigned short)FBLK_BASE+*ptr_to_data,src_buffer) != 0) {
					printf("Read: error, disk_read error\n");
					free_inode_blk_buffer(inode_num,&inode);
					return ngot;
				}
				if(nleft < (BLOCK_SIZE-blk_start_index)) {
					memcpy(buffer+ngot,(char*)src_buffer+blk_start_index,nleft);
					blk_start_index = 0;
					ngot += nleft;
					nleft -= nleft;
				}
				else {
					memcpy(buffer+ngot,(char*)src_buffer+blk_start_index,BLOCK_SIZE-blk_start_index);
					blk_start_index = 0;
					ngot += (BLOCK_SIZE - blk_start_index);
					nleft -= (BLOCK_SIZE - blk_start_index);
				}			
			}
			ptr1_start_index = 0;
		}
	}
	
	if(offset < (DIRP_MAX_SIZE+INDP_MAX_SIZE+DINDP_MAX_SIZE) && nleft > 0) {
		int ptr1_start_index,ptr2_start_index,blk_start_index;
		if(is_offset) {
			//ptr0_start_index = 0; doub_ind_pointer only one
			ptr1_start_index = (offset - DIRP_MAX_SIZE - INDP_MAX_SIZE) / (INDP_MAX_SIZE / 2);
			ptr2_start_index = (offset - DIRP_MAX_SIZE - INDP_MAX_SIZE - (ptr1_start_index * (INDP_MAX_SIZE / 2))) / BLOCK_SIZE;
			blk_start_index = (offset - DIRP_MAX_SIZE - INDP_MAX_SIZE - (ptr1_start_index * (INDP_MAX_SIZE / 2)) - (ptr2_start_index * BLOCK_SIZE));
			is_offset = 0;
		}
		else {
			ptr1_start_index = 0;
			ptr2_start_index = 0;
			blk_start_index = 0;
		}
		
		char tbuf1[BLOCK_SIZE];
		if(disk_read((unsigned short)FBLK_BASE+inode->doub_ind_pointer,tbuf1) != 0) {
			printf("Read: error, disk_read error\n");
			free_inode_blk_buffer(inode_num,&inode);
			return ngot;
		}
		unsigned short* ptr1_buffer = (unsigned short*)tbuf1;
		ptr1_buffer += ptr1_start_index;
		for(int i = ptr1_start_index;i < PTR_MAX_PBLK;i++,ptr1_buffer++) {
			if(*ptr1_buffer == ((unsigned short) -1)) {
				if(nleft > 0) {
					printf("Read: error, 文件写入时不按顺序\n");
					free_inode_blk_buffer(inode_num,&inode);
					return ngot;
				}
				break;
			}

			char tbuf2[BLOCK_SIZE];			
			if(disk_read((unsigned short)FBLK_BASE+*ptr1_buffer,tbuf2) != 0) {
				printf("Read: error, disk_read error\n");
				free_inode_blk_buffer(inode_num,&inode);
				return ngot;
			}
			unsigned short* ptr2_buffer = (unsigned short*)tbuf2; // ptr_to_data
			ptr2_buffer += ptr2_start_index;
			for(int j = ptr2_start_index; j < PTR_MAX_PBLK && nleft > 0;j++,ptr2_buffer++) {
				if( *ptr2_buffer == ((unsigned short)-1)) {
					if(nleft > 0) {
						printf("Read: error, 文件写入时不按顺序\n");
						free_inode_blk_buffer(inode_num,&inode);
						return ngot;
					}
					break;
				}
				char src_buffer[BLOCK_SIZE];
				if(disk_read((unsigned short)FBLK_BASE+*ptr2_buffer,src_buffer) != 0) {
					printf("Read: error, disk_read error\n");
					free_inode_blk_buffer(inode_num,&inode);
					return ngot;
				}
				if(nleft < (BLOCK_SIZE-blk_start_index)) {
					memcpy(buffer+ngot,(char*)src_buffer+blk_start_index,nleft);
					blk_start_index = 0;
					ngot += nleft;
					nleft -= nleft;
				}
				else {
					memcpy(buffer+ngot,(char*)src_buffer+blk_start_index,BLOCK_SIZE-blk_start_index);
					blk_start_index = 0;
					ngot += (BLOCK_SIZE - blk_start_index);
					nleft -= (BLOCK_SIZE - blk_start_index);
				}
			}
			ptr2_start_index = 0;
		}
		ptr1_start_index = 0;
	}
	
	printf("Read is called:%s\n",path);
	free_inode_blk_buffer(inode_num,&inode);
	return ngot;
}

/* fs_write - 
 *	分两部分写，一部分是追加写，一部分是原位更新
 * return value; return 0 if errors exits else size
 */

int fs_write (const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi){
	if (NULL == path || NULL == buffer || O_RDONLY == fi->flags){
		printf("error: path or buffer\n");
		return 0;
	}

	struct Inode *inode;
	int inode_num = get_inode(path, &inode);
	if (inode_num < 0){
		printf("error: get inode\n");
		free_inode_blk_buffer(inode_num, &inode);
		return 0;
	}

	// 分两部分写：一部分是append写，另一部分原位跟新
	// append 写，则先使文件大小变大

	// 计算append写的大小和块数
	// int write_to_blk(unsigned short* blk_ptr,size_t len,void* buffer,size_t size,off_t offset)
	// int alloc_blk(int inode_num, size_t append_size, unsigned short* blk_ptr, size_t *len,off_t *offset)

	size_t append_size = size + (size_t)offset  - inode->size;
	if ((fi->flags & (O_APPEND)) != 0 || append_size > 0){	//WWWW
		unsigned short* ptr_list = malloc(sizeof(unsigned short)*PTR_MAX_CNT);
		if(NULL == ptr_list) {
			printf("Write: malloc error\n");
			free(ptr_list);
			free_inode_blk_buffer(inode_num, &inode);
			return 0;
		}
		memset(ptr_list, -1, sizeof(unsigned short)*PTR_MAX_CNT);
		off_t offset_ = 0;
		size_t len_ = 0;
		if(alloc_blk(inode_num, append_size, ptr_list, &len_, &offset_) < 0) {	// 获取blk list
			printf("write: allock block error\n");
			free_inode_blk_buffer(inode_num, &inode);
			free(ptr_list);
			return 0;
		}
		void *append_buf = (void *)((char *)buffer + inode->size); // 追加写开始的buffer地址
		if(write_to_blk(ptr_list, len_ , append_buf, append_size, offset_) < 0) {
			printf("write: write to block error\n");
			free_inode_blk_buffer(inode_num,&inode);
			free(ptr_list);
			return 0;
		}
		free(ptr_list);
	}

	size_t fwrite_size = (append_size > 0)? size - append_size : size; // 原位更新的大小
	void *fwrite_buf = (void *)((char *)buffer + offset); // 原位更新开始的buffer地址
	if (fwrite_size != 0){
		// unsigned short ptr_list[PTR_MAX_CNT]; //WWWwWWWWWWWWWWWWWWWWWWW
		unsigned short* ptr_list = malloc(sizeof(unsigned short)*PTR_MAX_CNT);
		unsigned char ptr_bmap = inode->pointer_bmap;
		unsigned int ptr_cnt0 = ptr_bmap & 0xf;
		unsigned int ptr_cnt1 = (ptr_bmap >> 4) & 0x3;
		unsigned int ptr_cnt2 = ptr_bmap >> 6;
		size_t ptr_n = 0;


		//	下面是获得所有datablock 的ptr list
		for (int i = 0; i < (int)ptr_cnt0; i++, ptr_n ++){
			ptr_list[i] = inode->dir_pointer[i];
		}

		for (int i = 0; i < (int)ptr_cnt1; i++){
			char buf[BLOCK_SIZE + 1];
			if (disk_read(inode->ind_pointer[i]+(unsigned short)FBLK_BASE, buf)){
				printf("error: disk read\n");
				free(ptr_list);
				free_inode_blk_buffer(inode_num,&inode);
				return -1;		
			}
			
			unsigned short* dir_ptr = (unsigned short *)buf;
			for (int j = 0; j < PTR_MAX_PBLK; j++, ptr_n++){
				if (*dir_ptr == (unsigned short *)(-1)){
					break;
				}
				ptr_list[ptr_n] = *dir_ptr;
				dir_ptr++;
			}
		}
		char buf[BLOCK_SIZE + 1];
		if (ptr_cnt2 != 0){
			char ind_buf[BLOCK_SIZE + 1];
			if (disk_read(inode->doub_ind_pointer+(unsigned short)FBLK_BASE, buf)){
				printf("error: disk read\n");
				free(ptr_list);
				free_inode_blk_buffer(inode_num,&inode);
				return -1;		
			}
			unsigned short *ind_ptr = (unsigned short*)ind_buf;
			for (int i = 0; i < PTR_MAX_PBLK; i++){
				if (*ind_ptr == (unsigned short)(-1)){
					break;
				}
				char buf[BLOCK_SIZE + 1];
				if (disk_read((int)(*ind_ptr), buf)){
					printf("error: disk read\n");
					free(ptr_list);
					free_inode_blk_buffer(inode_num,&inode);
					return -1;
				}
				unsigned short *dir_ptr = (unsigned short *)buf;
				for (int j = 0; j < PTR_MAX_PBLK; j++, ptr_n++){
					if (*dir_ptr == (unsigned short)(-1)){
						break;
					}
					ptr_list[ptr_n] = *dir_ptr;
					dir_ptr ++;
				}
				ind_ptr++;
			}
		}
		
		// 进行原位更新
		int start_blk_n = (int)(offset/(off_t)BLOCK_SIZE);
		offset = offset%(off_t)BLOCK_SIZE;
		if(write_to_blk(ptr_list+start_blk_n , ptr_n , fwrite_buf, fwrite_size, offset) < 0) {
			printf("write: write to block error\n");
			free(ptr_list);
			free_inode_blk_buffer(inode_num,&inode);
			return 0;
		}
		free(ptr_list);
	}
	
	inode_num = get_inode(path, &inode);
	if (inode_num < 0){
		printf("error: get inode\n");
		free_inode_blk_buffer(inode_num, &inode);
		return 0;
	}
	// 更新inode的time和size并写入inode blk
	if (append_size > 0){
		inode->ctime = time(NULL);
		inode->mtime = inode->ctime;
	}
	else{
		inode->mtime = time(NULL);
	}
	inode->size = size;
	if (disk_write(inode_num/(int)INODE_NUM_PBLK + (int)INODE_BASE, inode - inode_num%(int)INODE_NUM_PBLK)){
		printf("error: disk write\n");
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}
	free_inode_blk_buffer(inode_num,&inode);
	printf("Write is called:%s\n",path);

	return size;
}


/* fs_truncate - revise size information of regular file
 * return 0 if no error exists, else -1
 */
int fs_truncate (const char *path, off_t size){
	// 检查路径和大小
	if (NULL == path || size < 0 || size > FILE_MAX_SIZE){
		printf("error: path or size %ld \n", size);
		return -1;
	}
	// 获取文件Inode
	struct Inode *inode;
	int inode_num = get_inode(path, &inode); 
	switch (inode_num){
		case -1:
			printf("error: disk read\n");
			free_inode_blk_buffer(inode_num,&inode);
			return -1;
			break;
		case -2:
			printf("error: path (%s)\n", path);
			free_inode_blk_buffer(inode_num,&inode);
			return -1;
			break;
		case -3:
			printf("error: path (%s)\n", path);
			free_inode_blk_buffer(inode_num,&inode);
			return -1;
			break;
		default:
			break;
	}
	//note: 可能存在的问题，创建新文件时，
	//		先写入Inode再执行truncate还是？
	//		如果不能保证顺序，可能无法得到inode

	// 文件存储空间变化的大小
	__off_t request_size = size - inode->size;
	if (request_size > 0){	//请求扩大文件 //还有一种情况为没有改变大小
		int fblk_request_num = (int)(request_size/((__off_t)BLOCK_SIZE)); // 需要的空闲块数量
		int fblk_num = get_fblk_num(); // 检查空闲块个数是否充足
		if (fblk_num == -1){
			free_inode_blk_buffer(inode_num,&inode);
			return -1;
		}
		else if(fblk_num < fblk_request_num){//note: free blocks total size等于size被允许吗？
			free_inode_blk_buffer(inode_num,&inode);
			return -ENOSPC;
		}


		unsigned short fblk_id;
		for (int i = 0; i < fblk_request_num; i++){// 请求fblk，得到id同时更新inode
			if (get_fblk_num(1, &fblk_id) == -1){
				printf("error: get free blocks num\n");
				free_inode_blk_buffer(inode_num,&inode);
				return -1;
			}
			// todo： 更新指针（封装个函数？）
			// 更新指针，inode和datablock bitmap
			// int update_inode_ptr(int mode, int num, int *ptr_list)
			if (inode_ptr_add(inode_num, 1, &fblk_id) < 0){
				printf("error: inode ptr opt\n");
				free_inode_blk_buffer(inode_num,&inode);
				return -1;
			}

		}

	}
	else if(request_size < 0){ // 文件减小的情况，则从某个指针开始，将后面的指针都删除
		int del_start_ptr_num = (int)(size/(off_t)BLOCK_SIZE) + !!(size % (off_t)BLOCK_SIZE);
		if (indoe_ptr_del(inode_num, del_start_ptr_num)){
			printf("error: inode del");
			free_inode_blk_buffer(inode_num,&inode);
			return -1;
		}
	}
	// 删去 // 不存在文件大小缩小的情况

	//更新ctime和size
	inode->size = size;
	inode->ctime = time(NULL);
	if (disk_write(inode_num/(int)INODE_NUM_PBLK + (int)INODE_BMAP_BASE, inode - (inode_num%(int)INODE_NUM_PBLK))){
		printf("error: disk write\n");
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}

	printf("Truncate is called:%s\n",path);
	free_inode_blk_buffer(inode_num,&inode);
	return 0;
}

/* 综合mknod和mkdir的函数
 * path: 文件路径; 
 * is_dir: 是目录则为1, 是普通文件则为0
 * 返回值:
 * -ENOSPC: 空间不足
 * 0: 其他错误
 * 1: 正常
 */
int mkfile (const char *path,int is_dir) { 
	//先判断inode_block和free_block还是否有剩余 (虽然free_block不会用)
		//可以调用ljt已经写好的接口
		//没有剩余的话返回-ENOSPC

	int fblk_cnt = get_fblk_num();
	int finode_cnt = bmap_cnt(INODE_BMAP_BASE);
	if(-1 == fblk_cnt || -1 == finode_cnt) {
		printf("Mkfile: error, count bmap fail\n");
	}
	if(0 == fblk_cnt || 0 == finode_cnt) {
		return -ENOSPC;
	}
	
	//先解析路径,将path分为父目录和文件(夹)
		//其中path的结尾带不带'/'得分类讨论
		//结尾带'/'且是is_dir
		//结尾不带'/'的,直接分类处理
	char parent_path[BLOCK_SIZE]; //长度只是为了能装下path
	char name[NAME_MAX_LEN+1]; //该文件的文件名
	strcpy(parent_path,path);
	size_t len = strlen(path);
	if(path[len-1] == '/') {
		if(!is_dir) {
			printf("Mkfile: error, path's a directory\n");
			return 0;
		}
		else {
			parent_path[len-1] = '\0';
			len--;
		}
	}
	
	char* end_ptr = strrchr(parent_path,'/'); //找到最后一个'/'的位置
	strcpy(name,end_ptr+1);
	//WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW: 下面很多抄这句的都错了, /a, 匹配到的/就是根目录了, 别删. /b/才需要删
	if(end_ptr != parent_path) {
		*end_ptr = '\0'; //该位置置为'\0'
	}
	else {
		*(end_ptr+1) = '\0';
	}

	//解析完路径,获取父目录的inode
	struct Inode* parent_inode;
	int parent_inode_num;
	parent_inode_num = get_inode(parent_path,&parent_inode);
	
	if( parent_inode_num < 0) {
		printf("Mkfile: get inode error\n");
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		return 0;
	}
	
	//获取父目录的inode
		//修改mtime和ctime
		//在父目录的DirPair块中,添加一项
		//这个函数还是和写入时一样,封装一下
		//判断父目录的块是否需要新分配块,封装一下
		//如果新分配块失败,也返回-ENOSPC
	time_t cur_time = time(NULL);
	
	int inode_num = find_finode();
	if(inode_num < 0) {
		printf("Mkfile: find free inode error\n");
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		return 0;
	}
	unsigned short* ptr_list = malloc(sizeof(unsigned short)*PTR_MAX_CNT);
	if(NULL == ptr_list) {
		printf("Mkfile: malloc error\n");
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		free(ptr_list);
		return 0;
	}
	memset(ptr_list,-1,sizeof(unsigned short)*PTR_MAX_CNT);
	
	len = 0; //前面定义过了 这里借用一下
	off_t offset = 0;
	struct DirPair tmp_dirpair; 
	tmp_dirpair.inode_num = inode_num; 
	strcpy(tmp_dirpair.name,name);
	
	if(alloc_blk(parent_inode_num,sizeof(struct DirPair),ptr_list,&len,&offset) < 0) {
		printf("Mkfile: allock block error\n");
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		free(ptr_list);
		return 0;
	}
	
	if(write_to_blk(ptr_list,len,&tmp_dirpair,sizeof(struct DirPair),offset) < 0) {
		printf("Mkfile: write to block error\n");
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		free(ptr_list);
		return 0;
	}
	
	parent_inode->mtime = cur_time;
	parent_inode->ctime = cur_time;
	parent_inode->size += sizeof(struct DirPair);
	//下面这句复制truncate末尾的, 更新inode
	if(disk_write(parent_inode_num/(int)INODE_NUM_PBLK + (int)INODE_BASE, parent_inode - (parent_inode_num%(int)INODE_NUM_PBLK))) {
		printf("Mkfile: disk write error\n");
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		return 0;
	}

	//获取空闲的inode,初始化时不分配块
		//修改相应的inode_bmap
		//填充inode的相关内容
	if(inode_num < 0) {
		printf("Mkfile: find free inode error\n");
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		free(ptr_list);
		return 0;
	}
	struct Inode * inode;
	if(get_inode_iblk(inode_num,&inode) < 0) {
		printf("Mkfile: get inode in block error\n");
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		free_inode_blk_buffer(inode_num,&inode);
		free(ptr_list);
		return 0;
	}	
	inode->mode = (is_dir) ? (DIRMODE) : (REGMODE);
	inode->atime = inode->mtime = inode->ctime = cur_time;
	inode->size = 0;
	inode->pointer_bmap = (unsigned char)((is_dir) ? 1 : 0); //如果是文件夹 那么预先分配一个空块
	
	if(is_dir) {
		unsigned short free_block_list[4]; //实际上只用1
		if(find_fblk(1,free_block_list) < 0) {
			printf("Mkfile: find free block error\n");
			free_inode_blk_buffer(parent_inode_num,&parent_inode);
			free_inode_blk_buffer(inode_num,&inode);
			free(ptr_list);
			return 0;
		}
		inode->dir_pointer[0] = free_block_list[0];
		inode->pointer_bmap = (unsigned char)1;
		char tbuf_[BLOCK_SIZE];
		if(disk_read(free_block_list[0]+(unsigned short)FBLK_BASE,tbuf_)) {
			printf("Mkfile: disk read error\n");
			free_inode_blk_buffer(parent_inode_num,&parent_inode);
			free_inode_blk_buffer(inode_num,&inode);
			free(ptr_list);
			return 0;
		}
		memset(tbuf_,-1,BLOCK_SIZE); //对于文件夹的块 需要初始化为-1
		if(disk_write(free_block_list[0]+(unsigned short)FBLK_BASE,tbuf_)) {
			printf("Mkfile: disk write error\n");
			free_inode_blk_buffer(parent_inode_num,&parent_inode);
			free_inode_blk_buffer(inode_num,&inode);
			free(ptr_list);
			return 0;
		}
	}
	//WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW: 下面这句可能错, 模仿ljt写的.// WWWWWWWWWWWWWWWW:加强制转换
	if(disk_write((unsigned short)INODE_BASE+inode_num/INODE_NUM_PBLK,inode - (inode_num % INODE_NUM_PBLK))) {
		printf("Mkfile: disk write error\n");
		free_inode_blk_buffer(parent_inode_num,&parent_inode);
		free_inode_blk_buffer(inode_num,&inode);
		free(ptr_list);
		return 0;
	}
	free_inode_blk_buffer(parent_inode_num,&parent_inode);
	free_inode_blk_buffer(inode_num,&inode);
	free(ptr_list);
	return 1;
}

/* 将数据写入块中的封装,凡是写入块的操作都可以调用这个
 * 使用方法: 先将inode中可用已分配好的指针存进数组blk_ptr里,
 * 在此函数外已经确保blk_ptr可存下size的数据!!!!!!,内部直接分配
 * 接口:
 * blk_ptr: 指针数组的首地址, 记录了要写入的块的指针, 按顺序写入其中
 * len: blk_ptr 数组的大小
 * buffer: 写入的内容
 * size: 需要写入的大小
 * offset: 在blk_ptr[0]的第几个byte开始写入
 * 返回值:
 * 	return -1 if errors exist, else 0
 */
int write_to_blk(unsigned short* blk_ptr,size_t len,void* buffer,size_t size,off_t offset) {
	if (NULL == blk_ptr || 0 == len || NULL == buffer || 0 == size || offset < 0)	// 参数有错则退出
		return -1;

	if (((size_t)BLOCK_SIZE * len) < size) // 大小不够则退出
		return -1;
	
	// 分为两种写：有偏移和无偏移
	if (offset > 0){	// 有偏移
		char write_buf[BLOCK_SIZE + 1];
		// 写第一个
		if (disk_read(*blk_ptr + FBLK_BASE, write_buf)){
			printf("error: disk read\n");
			return -1;
		}
		
		int memcpy_size = size+offset > BLOCK_SIZE? BLOCK_SIZE-offset :size;
		memcpy((void *)(write_buf + offset), buffer, (size_t)(memcpy_size));	//复制offset开始位置之前的内容
		
		if (disk_write(*blk_ptr + FBLK_BASE, write_buf)){	// 写入第一个块
			printf("error: disk write\n");
			return -1;
		}
		len--;
		blk_ptr++;	// blk_ptr 指向下一个块
		buffer = (void*)((char *)buffer + (int)BLOCK_SIZE - offset);	// buffer偏移
		// 写中间块
		for (int i = 1; i < (int)len-1; i++){
			if (disk_write(*blk_ptr + FBLK_BASE, buffer)){
				printf("error: disk write\n");
				return -1;
			}
			len--;
			blk_ptr++;
			buffer = (void*)((char *)buffer + (int)BLOCK_SIZE);
		}
		// 写最后一个
		if (len > 0){
			if (disk_read(*blk_ptr + FBLK_BASE, write_buf)){
				printf("error: disk read\n");
				return -1;
			}
			memset(write_buf, -1, BLOCK_SIZE);/////////////////////////////////////////////////
			memcpy((void *)(write_buf), buffer, (size_t)(offset));
			if (disk_write(*blk_ptr + FBLK_BASE, write_buf)){
				printf("error: disk write\n");
				return -1;
			}
		}
			

	}
	else{
		size_t write_size = size;
		for (int i = 0; i < len; i++){
			char buffer_[BLOCK_SIZE + 1];
			if (disk_read(*blk_ptr + FBLK_BASE, buffer_)){
				printf("error: disk read\n");
			}
			size_t memcpy_size = (write_size > (size_t)BLOCK_SIZE)? BLOCK_SIZE:write_size;
			memcpy(buffer_, buffer, memcpy_size);
			if (disk_write(*blk_ptr + FBLK_BASE, buffer_)){
				printf("error: disk write\n");
				return -1;
			}
			blk_ptr++;
			write_size = write_size - (size_t)BLOCK_SIZE;
			buffer = (void*)((char *)buffer + (int)BLOCK_SIZE);
		}
	}
	return 0;

}

/* 配合写入块时的辅助函数, 判断是否需要分配新的块,如果需要则分配
 * 分配完块后, 相应的指针, 指针块都有修改
 * 注意!: 不更改inode的size
 * 注意! 新块全初始化为-1, 为方便文件夹使用(也可判断)
 * 接口:
 * inode: 即文件的inode
 * size: 需要新增的size
 * blk_ptr: 存储appended_size的blk_ptr
 * len: blk_ptr的长度
 * offset: blk_ptr[0]指向的块中开始写入的位置
 * 实际上后三个参数通过指针传参,即需要修改.
 * 返回值:
 * 新分配的块的数目(不一定等于len)
 * 或分配失败,无多余空间等错误,返回-1
 */
int alloc_blk(int inode_num,size_t append_size,unsigned short* blk_ptr,size_t *len,off_t *offset) {
	//WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW: haven't review
	//先计算需要多少个free blk分配
	struct Inode* inode;
	int errFlag;
	if( (errFlag = get_inode_iblk(inode_num,&inode)) < 0){
		printf("Alloc blk: error, get_inode_iblk\n");
		free_inode_blk_buffer(inode_num,&inode);
		return -1;
	}
	unsigned char ptr_bmap = inode->pointer_bmap;
	unsigned int ptr_cnt0 = ptr_bmap & 0xf;
	unsigned int ptr_cnt1 = (ptr_bmap >> 4) & 0x3;
	unsigned int ptr_cnt2 = ptr_bmap >> 6;
	int cur_size = inode->size;
	int cur_blk = allocated_blk_cnt(inode); // 当前块数目 !!!!!!!!!!!! EEEEEEEEEEE: 初始化时不是这样的
	if(0 == cur_blk && 0 != ptr_cnt0) {
		cur_blk = 1; //即文件夹初始化时, 是一个空块, 但整个块都可以用
	}
	int capacity = cur_blk * BLOCK_SIZE; //当前总容量
	int cur_fspc = capacity - cur_size; //current free space
	int needed_fspc = append_size - cur_fspc;
	int needed_spc = append_size - cur_fspc; //needed space
	int needed_fblk = (needed_spc / BLOCK_SIZE) + !!(needed_spc % BLOCK_SIZE);

	int cur_ptr_flag = 0; //现在分配到哪个指针了, 也可以通过休闲的ptr_cnt来判断
	if(cur_size < DIRP_MAX_SIZE) 
		cur_ptr_flag = 0;
	else if(cur_size < DIRP_MAX_SIZE + INDP_MAX_SIZE)
		cur_ptr_flag = 1;
	else 
		cur_ptr_flag = 2;

	// 无需再分配块
	if(append_size < cur_fspc) {
		*len = 1;
		*offset = BLOCK_SIZE - cur_fspc; //即最后一个未满的块 - 剩余空间. 
		unsigned int ptr0_start_index,ptr1_start_index,ptr2_start_index;
		char tbuffer[BLOCK_SIZE];
		unsigned short* ptr_buffer = (unsigned short*) tbuffer;
		char tbuf1[BLOCK_SIZE];
		unsigned short* ptr1_buf = (unsigned short*) tbuf1;
		char tbuf2[BLOCK_SIZE];
		unsigned short* ptr2_buf = (unsigned short*) tbuf2;

		switch (cur_ptr_flag) {
			case 0:
				blk_ptr[0] = inode->dir_pointer[ptr_cnt0-1];
				break;
			case 1:
				ptr0_start_index = ptr_cnt1 - 1;
				if(disk_read((unsigned short)FBLK_BASE + inode->ind_pointer[ptr0_start_index],tbuffer)) {
					printf("Alloc blk: error, disk_read error\n");
					free_inode_blk_buffer(inode_num,&inode);
					return -1;
				}
				
				ptr1_start_index =  cur_blk - ptr_cnt0 - (ptr_cnt1-1) * PTR_MAX_PBLK - 1; 
				// 似乎还可以 ptr1_start_index = (cur_blk - ptr_cnt0 - 1) / (BLOCK_SIZE / sizeof(unsigned short));
				//WWWWWWWWWWWWWWWW: 极有可能写错上面这句 (BLOCK_SIZE / 2)意思是一个block里含义多少指针, 末尾-1是改为offset
				blk_ptr[0] = ptr_buffer[ptr1_start_index];
				break;
			case 2:
				
				if(disk_read((unsigned short)FBLK_BASE + inode->doub_ind_pointer,tbuf1)) {
					printf("Alloc blk: error, disk_read error\n");
					free_inode_blk_buffer(inode_num,&inode);
					return -1;
				}
				
				ptr1_start_index = (cur_blk - 1 - ptr_cnt0 - ptr_cnt1 * PTR_MAX_PBLK) / PTR_MAX_PBLK;
				
				
				if(disk_read((unsigned short)FBLK_BASE + *(ptr1_buf + ptr1_start_index),tbuf2)) {
					printf("Alloc blk: error, disk_read error\n");
					free_inode_blk_buffer(inode_num,&inode);
					return -1;
				}
				
				ptr2_start_index = cur_blk - ptr_cnt1 - ptr_cnt2 * PTR_MAX_PBLK - (ptr1_start_index-1) * PTR_MAX_PBLK - 1; 
				blk_ptr[0] = ptr2_buf[ptr2_start_index];
				break;
			default:
				printf("Alloc blk: error, cannot go into here\n");
				break;
		}
		free_inode_blk_buffer(inode_num,&inode);
		return 0;
	}
	else {
		
		int blkptr_index = (0 == cur_fspc) ? 0 : 1;
		*len = (0 == cur_fspc) ? needed_fblk : (needed_fblk + 1);
		*offset = (0 == cur_fspc) ? 0 : (BLOCK_SIZE - cur_fspc);

		unsigned int ptr0_start_index,ptr1_start_index,ptr2_start_index;
		char tbuffer[BLOCK_SIZE];
		unsigned short* ptr_buffer = (unsigned short*) tbuffer;
		char tbuf1[BLOCK_SIZE];
		unsigned short* ptr1_buf = (unsigned short*) tbuf1;
		char tbuf2[BLOCK_SIZE];
		unsigned short* ptr2_buf = (unsigned short*) tbuf2;

		if(0 != cur_fspc) {
			//复制了上面的switch
			switch (cur_ptr_flag) {
				case 0:
					blk_ptr[0] = inode->dir_pointer[ptr_cnt0-1];
					break;
				case 1:
					ptr0_start_index = ptr_cnt1 - 1;
					
					if(disk_read((unsigned short)FBLK_BASE + inode->ind_pointer[ptr0_start_index],tbuffer)) {
						printf("Alloc blk: error, disk_read error\n");
						free_inode_blk_buffer(inode_num,&inode);
						return -1;
					}
					
					ptr1_start_index =  cur_blk - ptr_cnt0 - (ptr_cnt1-1) * PTR_MAX_PBLK - 1; 
					// 似乎还可以 ptr1_start_index = (cur_blk - ptr_cnt0 - 1) / (BLOCK_SIZE / sizeof(unsigned short));
					//WWWWWWWWWWWWWWWW: 极有可能写错上面这句 (BLOCK_SIZE / 2)意思是一个block里含义多少指针, 末尾-1是改为offset
					blk_ptr[0] = ptr_buffer[ptr1_start_index];
					break;
				case 2:
					
					if(disk_read((unsigned short)FBLK_BASE + inode->doub_ind_pointer,tbuf1)) {
						printf("Alloc blk: error, disk_read error\n");
						free_inode_blk_buffer(inode_num,&inode);
						return -1;
					}
					
					ptr1_start_index = (cur_blk - 1 - ptr_cnt0 - ptr_cnt1 * PTR_MAX_PBLK) / PTR_MAX_PBLK;
					
					
					if(disk_read((unsigned short)FBLK_BASE + *(ptr1_buf + ptr1_start_index),tbuf2)) {
						printf("Alloc blk: error, disk_read error\n");
						free_inode_blk_buffer(inode_num,&inode);
						return -1;
					}
					
					ptr2_start_index = cur_blk - ptr_cnt1 - ptr_cnt2 * PTR_MAX_PBLK - (ptr1_start_index-1) * PTR_MAX_PBLK - 1; 
					blk_ptr[0] = ptr2_buf[ptr2_start_index];
					break;
				default:
					printf("Alloc blk: error, cannot go into here\n");
					break;
			}
		}

		//然后直接find_fblk,找到相应的数目
		if(find_fblk(needed_fblk,blk_ptr+blkptr_index) < 0) {
			printf("Alloc blk: error, find_fblk\n");
			free_inode_blk_buffer(inode_num,&inode);
			return -1;
		}

		//然后再用inode_ptr_add填进去
		if(inode_ptr_add(inode_num,needed_fblk,blk_ptr+blkptr_index) < 0) {
			printf("Alloc blk: error, find_fblk\n");
			free_inode_blk_buffer(inode_num,&inode);
			return -1;
		}
	}
	free_inode_blk_buffer(inode_num,&inode);
	return needed_fblk;
}

/* allocated_blk_cnt - allocated block count
 * 通过inode->size来判断现在已分配了多少个blk
 * 接口:
 * inode: 待判断的inode
 * 返回值:
 * 已分配的blk数
 */
int allocated_blk_cnt(struct Inode* inode) {
	return (inode->size / BLOCK_SIZE) + !!(inode->size % BLOCK_SIZE); 
	//考虑换成下面那个,如果需要
} 

int fs_mknod (const char *path, mode_t mode, dev_t dev)
{
	(void) mode;
	(void) dev;
	int retval = mkfile(path,0);
	if(0 == retval) {
		printf("Mknode: error\n");
	}
	retval = (retval > 0) ? 0 : retval;
	printf("Mknod is called:%s\n",path);
	return retval;
}

int fs_mkdir (const char *path, mode_t mode)
{
	(void) mode;
	int retval = mkfile(path,1);
	if(0 == retval) {
		printf("Mkdir: error\n");
	}
	retval = (retval > 0) ? 0 : retval;
	printf("Mkdir is called:%s\n",path);
	return retval;
}

int fs_rmdir (const char *path){
	if (rmfile(path)){
		printf("error: rm file\n");
		return -1;
	}
	printf("Rmdir is called:%s\n",path);
	return 0;
}

int fs_unlink (const char *path){
	
	if (rmfile(path)){
		printf("error: rm file\n");
		return -1;
	}
	
	printf("Unlink is callded:%s\n",path);
	return 0;
}

int fs_rename (const char *oldpath, const char *newname)
{
	//提取父目录
	char old_parent_path[BLOCK_SIZE];
	char new_parent_path[BLOCK_SIZE];
	char name[NAME_MAX_LEN+1];
	strcpy(old_parent_path,oldpath);
	strcpy(new_parent_path,newname);
	size_t old_len = strlen(old_parent_path);
	size_t new_len = strlen(new_parent_path);

	if(old_parent_path[old_len-1] == '/') {
		old_parent_path[old_len-1] = '\0';
		old_len--;
	}
	if(new_parent_path[new_len-1] == '/') {
		new_parent_path[new_len-1] = '\0';
		new_len--;
	}
	
	char* old_end_ptr = strrchr(old_parent_path,'/');
	if(old_end_ptr != old_parent_path) {
		*old_end_ptr = '\0';
	}
	else {
		*(old_end_ptr+1) = '\0';
	}

	char* new_end_ptr = strrchr(new_parent_path,'/');
	strcpy(name,new_end_ptr+1);
	if(new_end_ptr != new_parent_path) {
		*new_end_ptr = '\0';
	}
	else {
		*(new_end_ptr+1) = '\0';
	}
	
	//获取文件,俩父目录的inode
	int inode_num, new_parent_inode_num, old_parent_inode_num;
	struct Inode* inode;
	struct Inode* new_parent_inode;
	struct Inode* old_parent_inode;
	inode_num = get_inode(oldpath,&inode);
	old_parent_inode_num = get_inode(old_parent_path,&old_parent_inode);

	//删除原目录下的DirPair
	rm_file_in_parent_dir(oldpath);

	//添加到新目录下的DirPair
		//WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW:需要更新time吗? 暂时不更新
	new_parent_inode_num = get_inode(new_parent_path,&new_parent_inode); 
	//!!!!!!!!!!!!!!!!!: 这个一定得在删除了old_parent之后再读, 因为old可能和new的一样,更新了old后未落盘更新到new
	struct DirPair newpair;
	newpair.inode_num = inode_num;
	strcpy(newpair.name,name);
	size_t len = 0;
	off_t offset = 0;
	unsigned short* ptr_list = malloc(sizeof(unsigned short)*PTR_MAX_CNT);
	if(NULL == ptr_list) {
		free_inode_blk_buffer(inode_num,&inode);
		free_inode_blk_buffer(old_parent_inode_num,&old_parent_inode);
		free_inode_blk_buffer(new_parent_inode_num,&new_parent_inode);
		free(ptr_list);
		printf("Rename: malloc error\n");
		return -1;
	}
	memset(ptr_list,-1,sizeof(unsigned short)*PTR_MAX_CNT);

	if(alloc_blk(new_parent_inode_num,sizeof(struct DirPair),ptr_list,&len,&offset) < 0) {
		printf("Rename: alloc block error\n");
		free_inode_blk_buffer(inode_num,&inode);
		free_inode_blk_buffer(old_parent_inode_num,&old_parent_inode);
		free_inode_blk_buffer(new_parent_inode_num,&new_parent_inode);
		free(ptr_list);
		return -ENOSPC;
	}

	if(write_to_blk(ptr_list,len,&newpair,sizeof(struct DirPair),offset) < 0) {
		printf("Rename: write to block error\n");
		free_inode_blk_buffer(inode_num,&inode);
		free_inode_blk_buffer(old_parent_inode_num,&old_parent_inode);
		free_inode_blk_buffer(new_parent_inode_num,&new_parent_inode);
		free(ptr_list);
		return -1;
	}
	
	new_parent_inode->size += sizeof(struct DirPair);
	
	//抄ljt的
	if (disk_write(new_parent_inode_num/(int)INODE_NUM_PBLK + (int)INODE_BASE, new_parent_inode - new_parent_inode_num%(int)INODE_NUM_PBLK)){
		printf("error: disk write\n");
		free(ptr_list);
		free_inode_blk_buffer(inode_num,&inode);
		free_inode_blk_buffer(old_parent_inode_num,&old_parent_inode);
		free_inode_blk_buffer(new_parent_inode_num,&new_parent_inode);
		return -1;
	}

	free_inode_blk_buffer(inode_num,&inode);
	free_inode_blk_buffer(old_parent_inode_num,&old_parent_inode);
	free_inode_blk_buffer(new_parent_inode_num,&new_parent_inode);
	free(ptr_list);
	return 0;
}

int fs_utime (const char *path, struct utimbuf *buffer)
{
	struct Inode* inode;
	int inode_num = get_inode(path,&inode);
	if(inode_num < 0) {
		printf("Utime: get inode error\n");
		free_inode_blk_buffer(inode_num,&inode);
		return 0;
	}
	
	inode->ctime = time(NULL);
	inode->atime = buffer->actime;
	inode->mtime = buffer->modtime;

	if(disk_write(INODE_BASE + inode_num / INODE_NUM_PBLK, inode - (inode_num % INODE_NUM_PBLK))) {
		printf("Utime: disk write error\n");
		free_inode_blk_buffer(inode_num,&inode);
		return 0;
	}

	printf("Utime is called:%s\n",path);
	free_inode_blk_buffer(inode_num,&inode);
	return 0;
}


int fs_open (const char *path, struct fuse_file_info *fi)
{
	printf("Open is called:%s\n",path);
	return 0;
}

//Functions you don't actually need to modify
int fs_release (const char *path, struct fuse_file_info *fi)
{
	printf("Release is called:%s\n",path);
	return 0;
}

int fs_opendir (const char *path, struct fuse_file_info *fi)
{
	printf("Opendir is called:%s\n",path);
	return 0;
}

int fs_releasedir (const char * path, struct fuse_file_info *fi)
{
	printf("Releasedir is called:%s\n",path);
	return 0;
}

static struct fuse_operations fs_operations = {
	.getattr    = fs_getattr,
	.readdir    = fs_readdir,
	.read       = fs_read,
	.mkdir      = fs_mkdir,
	.rmdir      = fs_rmdir,
	.unlink     = fs_unlink,
	.rename     = fs_rename,
	.truncate   = fs_truncate,
	.utime      = fs_utime,
	.mknod      = fs_mknod,
	.write      = fs_write,
	.statfs     = fs_statfs,
	.open       = fs_open,
	.release    = fs_release,
	.opendir    = fs_opendir,
	.releasedir = fs_releasedir
};

int main(int argc, char *argv[])
{
	if(disk_init())
		{
		printf("Can't open virtual disk!\n");
		return -1;
		}
	if(mkfs())
		{
		printf("Mkfs failed!\n");
		return -2;
		}
    return fuse_main(argc, argv, &fs_operations, NULL);
}
