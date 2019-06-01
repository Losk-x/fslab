/*
Filesystem Lab disigned and implemented by Liang Junkai,RUC
*/

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
 * else return num
 */
int get_inode(const char* path,struct Inode **target) {
	// 不包含根目录，返回-3
	if (strchr(path, '/') == NULL) //////////////////开局就错= =， strchr找不到返回的是NULL，应该是 == NULL才return
		return -3;
	// 只含根目录
	struct Inode* inode_buf;
	if ((*path == '/') && (strlen(path) == 1)){
		if (get_inode_iblk(ROOT_I, &inode_buf) == -1)
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
 * 其他情况,返回0
 */
int get_inode_iblk(int inode_num, struct Inode** inode ){
    // calculate block id and offset within a block where inode locates
    int blk_id = INODE_BASE + inode_num/INODE_NUM_PBLK;
    int blk_offset = INODE_SIZE * (inode_num % INODE_NUM_PBLK);

    char buf[BLOCK_SIZE+1];
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

/* find_fblk - find free blocks using first fit algorithm
 * num: request free blocks number
 * fblk_list: used to store free block numbers
 * return (free blocks num - num) if no errors exist
 * return -1 if free blocks num less than num
 * return -3 if num == 0 or fblk_list == NULL
 * else return -2 
 */
int find_fblk(int num, int *fblk_list){
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
	for (int i = 0; i < BLOCK_SIZE; i++){
		for (int j = 0; j < 8; j++){
			// 未被占用则赋值给fblk_list
			if (0 == ((buf[i]>>j) & 0x1)){
				num--;
				(*fblk_list++) = blk_id; 
			}
			// 分配完成则返回
			if (num == 0){
				return fblk_num;
			}
			blk_id++;
		}
	}
	// 读取第二个freeblockk bitmap
	if (disk_read(FBLK_BMAP_BASE+1, buf)){
		printf("error: disk read %d\n", FBLK_BMAP_BASE);
		return -2;
	}
	// 按位检查
	for (int i = 0; i < BLOCK_SIZE; i++){
		for (int j = 0; j < 8; j++){
			// 未被占用则赋值给fblk_list
			if (0 == ((buf[i]>>j) & 0x1)){
				num--;
				(*fblk_list++) = blk_id; 
			}
			// 分配完成则返回
			if (num == 0){
				return fblk_num;
			}
			blk_id++;
		}
	}
	
	return fblk_num;

}

/* inode_ptr_opt - free or allocate block pointer in the inode
 * mode: 1 for allocating ptr, 0 for freeing ptr
 * ptr_list: ptr needed to be removed or added from inode
 * return -1 if errors exist, else 0
 */
//WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW:int*ptr_list, 虽然暂时没问题,但在将ptr的block用memcpy拷贝进这个ptr_list的时候会有问题,建议修改为short*
//WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW:尚未review完, 问ptr_list中的需要已经处理完bmap了吗?,在何处处理?,问pointer_bmap处理了吗?
int inode_ptr_opt(int mode, int inode_num, int ptr_num, int *ptr_list){
	(void)mode; //WWWWWWWWWWWWWWWWWWWWWWWWW, 没用到? 为啥还传进来
	struct Inode* inode;
	if (get_inode_iblk(inode_num, &inode)){
		printf("error: get inode %d\n", inode_num);
		return -1;
	}
	// 空指针或请求数量为0, 直接退出
	if ((NULL == ptr_list) || (ptr_num == 0)){
		return 0;
	}

	int dir_ptr_num = (dir_inode->pointer_bmap & 0xf);
    int ind_ptr_num = (dir_inode->pointer_bmap & 0x30) >> 4;
    int dual_ind_ptr_num = (dir_inode->pointer_bmap & 0x40) >> 6;
	
	// 定义一个操作符
	// free 000000
	// allocate 111111
	// x^(-1)=~x ~x
	
	// allocated or free blocks ptr
	// allocated blocks
	unsigned short *blk_ptr = inode->dir_pointer; //direct ptr
	for (int i = 0; i < dir_ptr_num && 0 != ptr_num; i++, blk_ptr++) {
		if (*blk_ptr != (unsigned short)(-1)){
			i--; //EEEEEEEEEEEEEEEEEEEEEEEEEE: 注意上面是i++,blk_ptr++, 此处blk_ptr未--, 建议改成成功后在循环末尾++这俩
			continue;
		}
		*blk_ptr = (unsigned short)(*ptr_list++);//赋值给这个空的指针 //WWWWWWWWWWWWWWWWWW: 这种有风险的*p++,建议还是加上括号
		ptr_num--;
		//EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE: 下面这个disk_write, 我晕了, 应该是错的把?????, 是按block写入,没有offset一说的.
		//WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW: 我就看到这里
		if (disk_write((int)INODE_BASE + inode_num/(int)INODE_NUM_PBLK, inode - inode_num%(int)INODE_NUM_PBLK)){//更新Inode
			printf("error: disk write, blk:%d\n", (int)INODE_BASE + inode_num/(int)INODE_NUM_PBLK);
			return -1;
		}
		int blk_num = (int)(*blk_ptr);//更新bitmap
		if (bitmap_opt(1, blk_num%((int)BLOCK_SIZE * 8), (int)FBLK_BMAP_BASE+blk_num/((int)BLOCK_SIZE * 8))){
			printf("error: bitmap opt, blk_num:%d\n", blk_num);
			return -1;
		}
	}
	blk_ptr = inode->ind_pointer;
	for (int i = 0; i < ind_ptr_num && 0!=ptr_num ;i++, blk_ptr++{//indirect ptr
		if (*blk_ptr == (unsigned short)(-1)){//空的indirect指针，则寻找空块，更新它
			int ind_ptr_block;
			if(find_fblk(1, &ind_ptr_block)){
				printf("error: find free blk\n");
				return -1;
			}
			*blk_ptr = (unsigned short)ind_ptr_block;//给该indirect指针赋值
			if (disk_write((int)INODE_BASE + inode_num/(int)INODE_NUM_PBLK, inode - inode_num%((int)INODE_NUM_PBLK))){//更新Inode
				printf("error: disk write, blk:%d\n", (int)INODE_BASE + inode_num/(int)INODE_NUM_PBLK);
				return -1;
			}
			int blk_num = (int)(*blk_ptr);//更新bitmap
			if (bitmap_opt(1, blk_num%(BLOCK_SIZE * 8), (int)FBLK_BMAP_BASE+blk_num/((int)BLOCK_SIZE * 8))){
				printf("error: bitmap opt, blk_num:%d\n", blk_num);
				return -1;
			}
			//再来一次
			i--;
			blk_ptr--;
		}
		else{//indirect指针非空，则在里面寻找可以更新的地方
			char ptr_buf[BLOCK_SIZE + 1];
			if (disk_read(*blk_ptr + (unsigned short)FBLK_BASE, ptr_buf)){
				printf("error: disk read\n");
				return -1;
			}
			unsigned short *short_ptr = (unsigned short)ptr_buf;
			for (int j = 0; j < PTR_MAX_PBLK && 0!=ptr_num; j++, short_ptr++){
				if (*short_ptr != (unsigned short)(-1)){
					continue;
				}
				*short_ptr = (unsigned short)(*ptr_list++);//赋值给这个空的指针
				ptr_num--;
				if (disk_write((int)FBLK_BASE + (int)*blk_ptr, ptr_buf)){//更新ind ptr block
					printf("error: disk write, blk:%d\n", INODE_BASE + inode_num/INODE_NUM_PBLK);
					return -1;
				}
				int blk_num = (int)(*short_ptr);//更新bitmap
				if (bitmap_opt(1, blk_num%(BLOCK_SIZE * 8), (int)FBLK_BMAP_BASE+blk_num/((int)BLOCK_SIZE * 8))){
					printf("error: bitmap opt, blk_num:%d\n", blk_num);
					return -1;
				}
			}
		}
	}
	if (0!=ptr_num){//到double indirect pointer仍还有指针没有分配完
		blk_ptr = &inode->doub_ind_pointer;
		if (*blk_ptr == (unsigned short)(-1)){//double indirect pointer为空，则给它赋值
			int ind_ptr_block;
			if(find_fblk(1, &ind_ptr_block)){
				printf("error: find free blk\n");
				return -1;
			}
			*blk_ptr = (unsigned short)ind_ptr_block;//给double indirect指针赋值
			if (disk_write((int)INODE_BASE + inode_num/(int)INODE_NUM_PBLK, inode - inode_num%((int)INODE_NUM_PBLK))){//更新Inode
				printf("error: disk write, blk:%d\n", (int)INODE_BASE + inode_num/(int)INODE_NUM_PBLK);
				return -1;
			}
			int blk_num = (int)(*blk_ptr);//更新bitmap
			if (bitmap_opt(1, blk_num%(BLOCK_SIZE * 8), (int)FBLK_BMAP_BASE+blk_num/((int)BLOCK_SIZE * 8))){
				printf("error: bitmap opt, blk_num:%d\n", blk_num);
				return -1;
			}
		}

		char dual_ind_buf[BLOCK_SIZE + 1];
		if (disk_read((int)*blk_ptr + (int)FBLK_BASE, dual_ind_buf)){
			printf("error: disk read\n");
			return -1;
		}
		blk_ptr = (unsigned short*)dual_ind_buf;
		for (int i = 0; i < ind_ptr_num && 0!=ptr_num ;i++, blk_ptr++{//indirect ptr
			if (*blk_ptr == (unsigned short)(-1)){//空的indirect指针，则寻找空块，更新它
				int ind_ptr_block;
				if(find_fblk(1, &ind_ptr_block)){
					printf("error: find free blk\n");
					return -1;
				}
				*blk_ptr = (unsigned short)ind_ptr_block;//给该indirect指针赋值
				if (disk_write((int)FBLK_BASE + (int)inode->doub_ind_pointer, dual_ind_buf)){//更新ind block
					printf("error: disk write, blk:%d\n", INODE_BASE + inode_num/INODE_NUM_PBLK);
					return -1;
				}
				int blk_num = (int)(*blk_ptr);//更新bitmap
				if (bitmap_opt(1, blk_num%(BLOCK_SIZE * 8), FBLK_BMAP_BASE+blk_num/(BLOCK_SIZE * 8))){
					printf("error: bitmap opt, blk_num:%d\n", blk_num);
					return -1;
				}
				//再来一次
				i--;
				blk_ptr--;
			}
			else{//indirect指针非空，则在里面寻找可以更新的地方
				char ptr_buf[BLOCK_SIZE + 1];
				if (disk_read(*blk_ptr + (unsigned short)FBLK_BASE, ptr_buf)){
					printf("error: disk read\n");
					return -1;
				}
				unsigned short *short_ptr = (unsigned short)ptr_buf;
				for (int j = 0; j < PTR_MAX_PBLK && 0!=ptr_num; j++, short_ptr++){
					if (*short_ptr != (unsigned short)(-1)){
						continue;
					}
					*short_ptr = (unsigned short)(*ptr_list++);//赋值给这个空的指针
					ptr_num--;
					if (disk_write((int)FBLK_BASE + (int)*blk_ptr, ptr_buf)){//更新ptr block
						printf("error: disk write, blk:%d\n", INODE_BASE + inode_num/INODE_NUM_PBLK);
						return -1;
					}
					int blk_num = (int)(*short_ptr);//更新bitmap
					if (bitmap_opt(1, blk_num%(BLOCK_SIZE * 8), FBLK_BMAP_BASE+blk_num/(BLOCK_SIZE * 8))){
						printf("error: bitmap opt, blk_num:%d\n", blk_num);
						return -1;
					}
				}
			}
		}		
	}

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
	inode->pointer_bmap = (unsigned char)0;
	
	#ifdef DEBUG
	inode->pointer_bmap = (unsigned char)1;/////测试read，不测试赋值为0
	inode->dir_pointer[0] = 0;/////测试read，不测试时删去, ERROR, should be zero
	if (bitmap_opt(1, 0, FBLK_BMAP_BASE)){
		printf("error: bmap opt\n");
		return -1;
	}
	#endif

	if (disk_write(INODE_BASE, buf)){
		printf("error: disk write error\n");
		return -1;
	}

	#ifdef DEBUG
	////////////support for read函数，root先初始化非空
	/////test file:"/x.c" (inode num:0)
	/////root data block:FBLK_BASE
	// if (bitmap_opt(1, 0, INODE_BMAP_BASE)){
	// 	printf("error: inode bmap opt\n");
	// 	return -1;	
	// }
	// if (bitmap_opt(1, 0, FBLK_BMAP_BASE)){
	// 	printf("error: db bmap opt\n");
	// 	return -1;		
	// }
	// /////上面的不要应该不影响
	// 将"x.c"dir pair写入root 的db中
	memset(buf, -1, BLOCK_SIZE+1); //////这里也应该初始化为-1  ////写入datablock时buffer都应去置为-1
	struct DirPair* dir_pair = (struct DirPair*) buf;
	dir_pair->inode_num = 0;
	char filename[]="x.c";
	memcpy(&(dir_pair->name), filename, strlen(filename)); ////////////// '\0'
	dir_pair->name[strlen(filename)] = '\0';
	
	if (disk_write(FBLK_BASE, buf)){
		printf("error: disk wirte\n");
		return -1;
	}

	// 将"x.c"的inode写入inode blocks
	if (bitmap_opt(1, 0, INODE_BMAP_BASE)){
		printf("error: bmap opt\n");
		return -1;
	}
	// write test file inode into data block
	if (disk_read(INODE_BASE, buf)){
		printf("error: disk read\n");
		return -1;
	}
	inode = ((struct Inode *)buf); 
	inode->mode = (__mode_t)REGMODE;
	inode->size = (__off_t)8;
	inode->atime = time(NULL);
	inode->ctime = inode->atime;
	inode->mtime = inode->atime;
	inode->pointer_bmap = (unsigned char)1;/////测试read
	inode->dir_pointer[0] = 1;/////测试read
	if (disk_write(INODE_BASE, buf)){
		printf("error: disk write error\n");
		return -1;
	}

	if (bitmap_opt(1, 1, FBLK_BMAP_BASE)){
		printf("error: bmap opt\n");
		return -1;
	}
	memset(buf,0,BLOCK_SIZE+1);
	memcpy(buf,"ljtnb!!!",8);
	if (disk_write(FBLK_BASE+1, buf)){
		return -1;
	}
	////////修改了root inode的dir_pointer和pointer_bmap
	///////////没有修改上面super block的fblk, ffree,仅供测试read
	#endif
	printf("Mkfs is called\n");
	return 0;
} 

//Filesystem operations that you need to implement

int fs_statfs (const char *path, struct statvfs *stat){
	(void) path;//没有意义的参数
	

	#ifdef DEBUG
	printf("stafs: is calling\n");
	#endif

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
        printf("error: path is NULL, path's \"%s\"\n",path);
        return -ENOENT;
    }

    struct Inode *inode;

	int errFlag = get_inode(path,&inode);

	switch (errFlag) {
		case -1:
			printf("error: disk read error, path's \"%s\"\n",path);
			return -ENOENT;
			break;
		case -2:
			printf("error: path doesn't exist, path's \"%s\"\n",path);
			return -ENOENT;
			break;
		case -3:
			printf("error: path error, path's \"%s\"\n",path);
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
	return 0;
}

int fs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi){
	#ifdef DEBUG
	printf("readdir: is calling\n");
	#endif
	
	(void) offset;
	(void) fi;
	// 判断路径是否为空，为空则报错
	if(NULL == path) {
        printf("error: path is NULL, path is \"%s\"\n",path);
        return -ENOENT;
    }
	// 路径正常，则读取path下dir的inode
	// inode的地址写入inode指针中
    struct Inode *dir_inode;
	int errFlag = get_inode(path, &dir_inode);
	switch (errFlag) {
		case -1:
			printf("error: disk read error, path is \"%s\"\n",path);
			return -ENOENT;
			break;
		case -2:
			printf("error: path doesn't exist, path is \"%s\"\n",path);
			return -ENOENT;
			break;
		case -3:
			printf("error: path error, path is \"%s\"\n",path);
			return -ENOENT;
			break;
		default:
			break;
	}
	// errFlag正常时即为dir_inode_num
	int dir_inode_num = errFlag;

	// 判断根据path读取的inode是否为目录
	if (dir_inode->mode == (__mode_t)(REGMODE)){
		printf("error: not a directory:%s (readdir)\n", path);
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
		printf("error: disk write\n");
		return -1;
	}

    int i, j, k, buf_offset = 0;
	char buf[BLOCK_SIZE+1];
	char file_path[256] = "";
    struct DirPair* dir_pair;
	//filler(buffer, "", NULL, 0);
	//filler(buffer, "..", NULL, 0);

    // read dir and compare file name to each dir_pair
	// 1. direct data block
    for (i = 0; i < dir_ptr_num; i++){
        // printf("readdir:  dir ptr\n");
	    // ptr used or not, if not, continue
        if (dir_inode->dir_pointer[i] == (unsigned short)(-1)){
            i--;
            continue;
        }
		// read in data block
		if (disk_read((unsigned short)FBLK_BASE + dir_inode->dir_pointer[i], buf))
			return -1;

		// initialize dir_pair ptr to the beginnning of the buffer
        dir_pair = (struct DirPair *)buf;
		// compare file name to dir_pairs in this blockm
        for (j = 0; j < DIR_PAIR_PBLK; j++, dir_pair++){
            // dir inode num initialize to -1
            if (dir_pair->inode_num == (-1))
                continue;

			// concat for file path
			// been tested
			memset(file_path, 0, sizeof(file_path));
			strcat(file_path, dir_path);
			strcat(file_path, dir_pair->name);
			
			struct stat attr;
			// call fs_getattr to get file stat
            // printf("readdir: %d %d %s %x\n", j, dir_pair->inode_num, dir_pair->name, dir_pair);
            if (fs_getattr(file_path ,&attr)){
				printf("error: fs_getattr\n");
				return -1;
			}
			
			// write file stat into buffer
			filler(buffer, dir_pair->name, NULL, 0);

        }
    }


	// 2. read indirect datablock
    for (i = 0; i < ind_ptr_num; i++){
		// if indirect pointer isn't used, continue to next indirect pointer
        // printf("readdir:  indir ptr\n");
        if (dir_inode->ind_pointer[i] == (unsigned short)(-1)){
            i--;
            continue;
        }
        char ptr_buf[BLOCK_SIZE + 1];
		// read in indirect pointer block
        if (disk_read((unsigned short)FBLK_BASE + dir_inode->ind_pointer[i], ptr_buf)) 
			return -1;

		// pointer number in the indirect pointer block
        // unsigned int ptr_num = (unsigned int)((short)ptr_buf);
		unsigned short *short_ptr = (unsigned short *)ptr_buf;
        for (j = 0; j < PTR_MAX_PBLK; j++){ 
            // if the pointer isn't used, continue to next pointer
			if (*(ptr_buf + j) == (unsigned short)(-1)){
                continue;
            }
            
			// read in direct pointer data block
			if (disk_read((unsigned short)FBLK_BASE + *(short_ptr + j), buf))
				return -1;
			
			dir_pair = (struct DirPair *)buf;
            for (k = 0; k < DIR_PAIR_PBLK; k++, dir_pair++){
                // if the dir_pair inode isn't used, continue to next dir_pair
				if (dir_pair->inode_num == -1)
                    continue;
				
				// concat for file path
				// been tested
				memset(file_path, 0, sizeof(file_path));
				strcat(file_path, dir_path);
				strcat(file_path, dir_pair->name);
				
				struct stat attr;
				// call fs_getattr to get file stat
				if (fs_getattr(file_path ,&attr)){
					printf("error: fs_getattr\n");
					return -1;
				}
				
				// write file stat into buffer
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
        if (disk_read((unsigned short)FBLK_BASE + dir_inode->doub_ind_pointer , dptr_buf))
			return -1;

		unsigned short *dshort_ptr = (unsigned short *)dptr_buf; 
		// for each indirect pointer, read datablock
        for (i = 0; i < PTR_MAX_PBLK; i++){ 
			// if this indirect pointer isn't used, continue
            if (*(dshort_ptr + i) == (unsigned short)(-1)){
                continue;
            }

			// read in indirect pointer datablock
            char ptr_buf[BLOCK_SIZE + 1];
            if (disk_read(((unsigned short)FBLK_BASE + *(dshort_ptr + i)), ptr_buf))
				return -1;

			// read in direct pointer datablock
			unsigned short *short_ptr = (unsigned short *)ptr_buf;
            for (j = 0; j < PTR_MAX_PBLK; j++){
				// if direct pointer isn't used, continue to next one
                if (*(short_ptr + j) == (unsigned short)(-1)){
                    continue;
                }
				// read in datablocks
                if (disk_read((unsigned short)FBLK_BASE + *(short_ptr + j), buf)) /////////warning: 没有检测回来的值是否是0/1
					return -1;
				dir_pair = (struct DirPair *)buf;
                for (k = 0; k < DIR_PAIR_PBLK; k++, dir_pair++){ 
					// if the dir_pair inode isn't used, continue to next dir_pair
					if (dir_pair->inode_num == -1)
						continue;
					
					
					// concat for file path
					// been tested
					memset(file_path, 0, sizeof(file_path));
					strcat(file_path, dir_path);
					strcat(file_path, dir_pair->name);
					
					struct stat attr;
					// call fs_getattr to get file stat
					if (fs_getattr(file_path ,&attr)){
						printf("error: fs_getattr\n");
						return -1;
					}
					
					// write file stat into buffer
					filler(buffer, dir_pair->name, NULL, 0);
                }
            }
        }
    }
	printf("Readdir is called:%s\n", path);
	return 0;
}

//WWWWWWWWWWWWWWWWWWWWWWWWW:尚未通过大文件测试(indirect 和 double indirect)
int fs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	//size means the req uired size
	#ifdef DEBUG
	printf("Read: is calling. Path:%s\n",path);
	#endif
	(void) fi; //消除unused
	struct Inode *inode;
	int errFlag = get_inode(path, &inode);
	
	switch (errFlag) {
		case -1:
			printf("error: disk read error, path's \"%s\"\n",path);
			return 0;
			break;
		case -2: 
			printf("error: path doesn't exist, path's \"%s\"\n",path);
			return 0;
			break; 
		case -3:
			printf("error: disk read error, path's \"%s\"\n",path);
			return 0;
			break;
		default:
			break;
	}
	//修改文件的atime
	inode->atime = time(NULL);

	if (inode->mode	== ((__mode_t)(DIRMODE)) ) { ///有需要吗?
		printf("Read: error, path's a directory. Path's \"%s\"\n",path);
		return 0;
	}
	if (offset > inode->size) {
		printf("Read: error, offset > real size of file. Path's \"%s\"\n",path);
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
					return ngot;
				}
				continue;
			}
			void *src_buffer;
			if(disk_read((unsigned short)FBLK_BASE + inode->dir_pointer[i],src_buffer) != 0) {
				printf("Read: error, disk_read error\n");
				return ngot;
			}
			
			if(nleft < (BLOCK_SIZE-blk_start_index)) {
				memcpy(buffer+ngot,(char*)src_buffer+blk_start_index,nleft);
				ngot += nleft;
				nleft -= nleft;
				blk_start_index = 0;	
			}
			else {
				memcpy(buffer+ngot,(char*)src_buffer+blk_start_index,BLOCK_SIZE-blk_start_index);
				blk_start_index = 0;
				ngot += (BLOCK_SIZE-blk_start_index);
				nleft -= (BLOCK_SIZE-blk_start_index);
			}
		}
	}
	//indirect pointer
	if(offset < (DIRP_MAX_SIZE+INDP_MAX_SIZE)) {
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
					return ngot;
				}
				continue;
			}
			char ptr_buffer[BLOCK_SIZE];
			if(disk_read((unsigned short)FBLK_BASE + inode->ind_pointer[i],ptr_buffer) != 0){
				printf("Read: error, disk_read error\n");
				return ngot;
			}
			unsigned short* ptr_to_data = (unsigned short*) ptr_buffer;
			ptr_to_data += ptr1_start_index;
			//写入时已经确保了一级指针块中,指针是连续的
			for(int j = ptr1_start_index;j < PTR_MAX_PBLK && nleft > 0;j++,ptr_to_data++) {
				if( *ptr_to_data == ((unsigned short)-1)) {
					if(nleft > 0) {
						printf("Read: error, 文件写入时不按顺序\n");
						return ngot;
					}
					break;
				}
				char src_buffer[BLOCK_SIZE];
				if(disk_read((unsigned short)FBLK_BASE+*ptr_to_data,src_buffer) != 0) {
					printf("Read: error, disk_read error\n");
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
	if(offset < (DIRP_MAX_SIZE+INDP_MAX_SIZE+DINDP_MAX_SIZE)) {
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
			return ngot;
		}
		unsigned short* ptr1_buffer = (unsigned short*)tbuf1;
		ptr1_buffer += ptr1_start_index;
		for(int i = ptr1_start_index;i < PTR_MAX_PBLK;i++,ptr1_buffer++) {
			if(*ptr1_buffer == ((unsigned short) -1)) {
				if(nleft > 0) {
					printf("Read: error, 文件写入时不按顺序\n");
					return ngot;
				}
				break;
			}

			char tbuf2[BLOCK_SIZE];			
			if(disk_read((unsigned short)FBLK_BASE+*ptr1_buffer,tbuf2) != 0) {
				printf("Read: error, disk_read error\n");
				return ngot;
			}
			unsigned short* ptr2_buffer = (unsigned short*)tbuf2; // ptr_to_data
			ptr2_buffer += ptr2_start_index;
			for(int j = ptr2_start_index; j < PTR_MAX_PBLK && nleft > 0;j++,ptr2_buffer++) {
				if( *ptr2_buffer == ((unsigned short)-1)) {
					if(nleft > 0) {
						printf("Read: error, 文件写入时不按顺序\n");
						return ngot;
					}
					break;
				}
				char src_buffer[BLOCK_SIZE];
				if(disk_read((unsigned short)FBLK_BASE+*ptr2_buffer,src_buffer) != 0) {
					printf("Read: error, disk_read error\n");
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
	return ngot;
}

int fs_write (const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi){
	#ifdef DEBUG
	printf("write: is calling\n");
	#endif



	printf("Write is called:%s\n",path);
	return 0;
}


/* fs_truncate - revise size information of regular file
 * return 0 if no error exists, else -1
 */
int fs_truncate (const char *path, off_t size){
	#ifdef DEBUG
	printf("truncate: is calling\n");
	#endif
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
			return -1;
			break;
		case -2:
			printf("error: path (%s)\n", path);
			return -1;
			break;
		case -3:
			printf("error: path (%s)\n", path);
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
		// 需要的空闲块数量
		int fblk_request_num = (int)(request_size/((__off_t)BLOCK_SIZE));
		// 检查空闲块个数是否充足
		int fblk_num = get_fblk_num();
		if (fblk_num == -1){
			return -1;
		}
		else if(fblk_num < fblk_request_num){//note: free blocks total size等于size被允许吗？
			return -ENOSPC;
		}

		//批量请求空闲块的num(16为一块)
		//或改为逐个逐个请求
		int fblk_list[16];//note: list的大小应该为多大？
		//请求得到空闲块的num，更新inode的指针
		for (int i = 0; i < fblk_request_num/16; i++){
			if (get_fblk_num(16, fblk_list) == -1){
				printf("error: get free blocks num\n");
				return -1;
			}
			fblk_request_num -= 16;

			// todo： 更新指针（封装个函数？）
			// 更新指针，inode和datablock bitmap
			// int update_inode_ptr(int mode, int num, int *ptr_list)

		}
		// 请求剩余的空闲块，更新指针
		fblk_request_num -= (fblk_request_num/16)*16;
		if (get_fblk_num(fblk_request_num, fblk_list) == -1){
			printf("error: get free blocks num\n");
			return -1;
		}
		// todo: 更新指针（封装函数）

	}
	// 删去 // 不存在文件大小缩小的情况
	// else if (request_size < 0){ //请求缩小文件
	// 	// 检查指针数量
	// 	// 先释放doub_ind_ptr，再释放ind_ptr，最后释放dir_ptr
	// 	// 即相当于更新指针
	// 	// 封装一个更新指针的函数很有必要
	// 	// int update_inode_ptr(int mode, int num, int *ptr_list)
		
	// 	// 得到释放的块数
	// 	request_size = -request_size;
	// 	int release_blk_num = (int)((request_size + (__off_t)BLOCK_SIZE - 1) / (__off_t)(BLOCK_SIZE));
	// 	// 释放相应数量的块及指针
	// 	// update_inode_ptr(0, release_blk_num, NULL);

	// }


	//更新ctime
	inode->ctime = time(NULL);
	if (disk_write(inode_num/INODE_NUM_PBLK + INODE_BMAP_BASE, inode - inode_num%INODE_NUM_PBLK)){
		printf("error: disk write\n");
		return -1;
	}

	printf("Truncate is called:%s\n",path);
	return 0;
}

/* 综合mknod和mkdir的函数
 * path: 文件路径; 
 * is_dir: 是目录则为1, 是普通文件则为0
 */
int mkfile (const char *path,int is_dir) {
	//先判断inode_block和free_block还是否有剩余 (虽然free_block不会用)
		//可以调用ljt已经写好的接口
		//没有剩余的话返回-ENOSPC

	//先解析路径,将path分为父目录和文件(夹)
		//其中path的结尾带不带'/'得分类讨论
		//结尾带'/'且是is_dir
		//结尾不带'/'的,直接分类处理
	

	//解析完路径,获取父目录的inode

	//获取父目录的inode
		//修改mtime和ctime
		//在父目录的DirPair块中,添加一项
		//这个函数还是和写入时一样,封装一下
		//判断父目录的块是否需要新分配块,封装一下
		//如果新分配块失败,也返回-ENOSPC

	//获取空闲的inode,初始化时不分配块
		//修改相应的inode_bmap
		//填充inode的相关内容

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
 * 
 */
int write_to_blk(unsigned short* blk_ptr,size_t len,void* buffer,size_t size,off_t offset) {

}

/* 配合写入块时的辅助函数, 判断是否需要分配新的块,如果需要则分配
 * 分配完块后, 相应的指针, 指针块都有修改
 * 注意! 新块全初始化为-1, 为方便文件夹使用(也可判断)
 * 接口:
 * inode: 即文件的inode
 * size: 需要新增的size
 * blk_ptr: 存储appended_size的blk_ptr
 * len: blk_ptr的长度
 * offset: blk_ptr[0]指向的块中开始写入的位置
 * 返回值:
 * 实际上后三个参数通过指针传参,即需要修改.
 * 新分配的块的数目:
 * 分配失败,无多余空间: 
 */
int alloc_blk(struct Inode* inode,size_t append_size,unsigned short* blk_ptr,size_t *len,off_t *offset) {
	//先计算需要多少个free blk分配
	int cur_size = inode->size;
	int cur_blk = allocated_blk_cnt(inode); // 当前块数目
	int capacity = cur_blk * BLOCK_SIZE; //当前总容量
	int cur_fspc = capacity - cur_size; //current free space
	int needed_fspc = append_size - cur_fspc;
	
	int cur_ptr_flag = 0; //现在分配到哪个指针了
	if(cur_size < DIRP_MAX_SIZE) 
		cur_ptr_flag = 0;
	else if(cur_size < DIRP_MAX_SIZE + INDP_MAX_SIZE)
		cur_ptr_flag = 1;
	else 
		cur_ptr_flag = 2;

	unsigned char ptr_bmap = inode->pointer_bmap;
	unsigned int ptr_cnt0 = ptr_bmap & 0xf;
	unsigned int ptr_cnt1 = (ptr_bmap >> 4) & 0x3;
	unsigned int ptr_cnt2 = ptr_bmap >> 6;

	if(append_size < cur_fspc) {
		*len = 1;
		*offset = BLOCK_SIZE - cur_fspc; //即最后一个未满的块 - 剩余空间.

		switch (cur_ptr_flag) {
			case 0:
				blk_ptr[0] = inode->dir_pointer[ptr_cnt0-1];
				break;
			case 1:
				
				break;
			case 2:
				break;
			default:
				break;
		}

		return 0;
	}
	else {
		int needed_spc = append_size - cur_fspc; //needed space
		int needed_fblk = needed_spc / BLOCK_SIZE + !!(needed_spc % BLOCK_SIZE);

	}

	//上面写的有点冗余, 现在开始逻辑先行
	//先计算需要多少个free blk分配
	

	if(needed_fspc < 0) {
		*len = 1;

		return 0; //此时默认最尾部的blk_ptr能存下appended_size
	}
	else {

	}
	//然后直接find_fblk,找到相应的数目,(尚未review,注意处理bmap)
	//然后再用inode_ptr_opt填进去
}

//WWWWWWWWWWWWWWWWWWWWWWWWWW: 下面这个函数可能对文件夹不适用!!!!!!
//且未检查readir的正确性!!! 
//由于分配策略的原因,目录的size不能指示其空位(需要再研究,通过first fit简单删除,由于删除时会有空位,可换成其他策略),
//在目录相关读写中,应该按pointer_bmap来读完所有的块,而非按size来判断
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
/* real_allocated_blk_cnt - real allocated block count
 * 通过inode的指针以及指针块中指针的具体值(非-1的指针数),来计算已分配的blk数
 * 接口:
 * inode: 待判定的inode
 * 返回值:
 * 已分配的blk数
 */
int real_allocated_blk_cnt(struct Inode* inode) {

}

int fs_mknod (const char *path, mode_t mode, dev_t dev)
{
	(void) mode;
	(void) dev;
	
	printf("Mknod is called:%s\n",path);
	return 0;
}
.
int fs_mkdir (const char *path, mode_t mode)
{
	(void) mode;

	printf("Mkdir is called:%s\n",path);
	return 0;
}

int fs_rmdir (const char *path)
{
	printf("Rmdir is called:%s\n",path);
	return 0;
}

int fs_unlink (const char *path)
{
	printf("Unlink is callded:%s\n",path);
	return 0;
}

int fs_rename (const char *oldpath, const char *newname)
{
	printf("Rename is called:%s\n",oldpath);
	return 0;
}

int fs_utime (const char *path, struct utimbuf *buffer)
{
	printf("Utime is called:%s\n",path);
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

/******************* DUMP *******************/
/******************* DUMP *******************/
/******************* DUMP *******************/
/******************* DUMP *******************/
/******************* DUMP *******************/
/******************* DUMP *******************/
/******************* DUMP *******************/

/*

//wrong, 但可以通过微调使用
//错误原因: 以为文件夹的路径名结尾必定带有'/'
//修改: 结尾的'/'改为'\0'，统一处理
//return -1 if fail to get, return -2 if name error, else return inode_num (ok)
int get_inode(const char* path,struct Inode *target) {
	size_t path_len = strlen(path);
	int dir_flag = (path[path_len-1] == '/'); //linux's '/' ; windows' '\\'
	const char *cur = strchr(path,'/');
	const char *prev = path;

	if (!cur) {
		return -1; //根目录都没有直接返回
	}
	void* buffer;

	if (disk_read(INODE_BASE,buffer)) {
		return -1; //读取异常 disk_read返回1
	}
	struct Inode* prevDir; // 注意此处用的是指针
	prevDir = ((struc Inode*)buffer)+ROOT_I;
	//上述后续可以用get_inode_iblk

	cur = strchr(cur+1,'/');
	int dir_inode_num;

	while(cur) { // cur = NULL then break (if not found '/')
		//先解析当前文件夹的名字
		//prev是上一个'/'，cur是这一个'/'，eg: '/usr/bin/‘ cur-prev = 4
		char cur_name[NAME_MAX_LEN+1];
		strncpy(cur_name,prev+1,cur-prev-1);
		cur_name[cur-prev-1] = '\0'; /////////////有错 0 base,cur-prev改成cur-prev-1
		//再到上一层的文件夹中去寻找
		dir_inode_num = find_inode_inDir(cur_name,prevDir);
		if (dir_inode_num < 0) {
			return dir_inode_num;
		}
		if(get_inode_iblk(dir_inode_num,prevDir) < 0) {
			return -1;
		}
		
		prev = cur;
		cur = strchr(cur+1,'/');		
		//在上一层的inode
	}
	// //处理有误，应该将文件夹视作文件来处理，将文件路径名最后的'/'去掉
	// if(!dir_flag) { // not a dir, thus need to get the file' inode
	// 	char cur_name[NAME_MAX_LEN+1];
	// 	strcpy(cur_name,prev+1);
	// 	int file_inode_num = find_inode_inDir(cur_name,prevDir);
	// 	if (file_inode_num < 0) {
	// 		return file_inode_num;
	// 	}
	// 	if (get_inode_iblk(file_inode_num,target) < 0) {
	// 		return -1;
	// 	}
	// 	return file_inode_num;
	// }
	// else {
	// 	target = prevDir;
	// 	return dir_inode_num;
	// }
}

// get_inode_idir的备胎
//备胎原因: get_inode中启用了另一套接口
//转正条件: 接口封装成，(const char* name,int dir_inode_num);
//find inode by name in the dir_blk
//name should be shorter than 24 bytes, dir_inode is the inode of the dir
//return -1 if wrong, return -2 if not found, else the inode_num of the name (>=0)
int find_inode_inDir(const char* name,struct Inode* dir_inode) {
	if(dir_inode->mode != DIRMODE) { //not a dir
		return -1;
	}
	int PBmap = dir_inode->pointer_bmap;
	int dirPCnt = PBmap & 0xf;
	int indPCnt = (PBmap >> 4) & 0x3;
	int dIndPCnt = PBmap >> 6;

	void *buf;
	//direct pointers
	for(int i = 0;i < dirPCnt;i++) {
		if (dir_inode->dir_pointer[i] == (unsigned short)(-1)){
            i--;
            continue;
        }
		if( disk_read(((unsigned short)FBLK_BASE) + dir_inode->dir_pointer[i], buf) ) { // == 1, wrong
			return -1;
		} 
		else { //== 0, right
			struct DirPair* dp = (struct DirPair*) buf;	
			for(int j = 0;j < DIR_PAIR_PBLK;j++) {
				if(dp->inode_num != -1) {
					if(strcmp(name,dp->name) == 0) {
						return dp->inode_num;
					} 
				}
				dp++;
			}
		}
	}
	//indirect pointers
	for(int i = 0;i < indPCnt;i++) {
		if (dir_inode->ind_pointer[i] == (unsigned short)(-1)){
            i--;
            continue;
        }
		if( disk_read(((unsigned short)FBLK_BASE) + dir_inode->ind_pointer[i],buf) ) { // == 1 wrong
			return -1;
		}
		else { //== 0, right
			unsigned short* short_ptr = (unsigned short*) buf;
			for(int j = 0;j < PTR_MAX_PBLK;j++) {
				void *tbuf;
				if( *short_ptr == ((unsigned short)-1) ) {
					continue;
				}
				if ( disk_read(((unsigned short)FBLK_BASE) + *short_ptr,tbuf) ) {
					return -1;
				}
				else {
					struct DirPair* dp = (struct DirPair*) tbuf;	
					for(int k = 0;k < DIR_PAIR_PBLK;k++) {
						if(dp->inode_num != -1) {
							if(strcmp(name,dp->name) == 0) {
								return dp->inode_num;
							} 
						}
						dp++;
					}					
				}
				short_ptr++;
			}
		}
	}
	//double indirect 
	if (dIndPCnt == 1) {
		if ( disk_read(((unsigned short)FBLK_BASE) + dir_inode->doub_ind_pointer,buf) ) {
			return -1;
		}
		unsigned short* short_ptr1 = (unsigned short*) buf;
		for(int i = 0;i < PTR_MAX_PBLK;i++) {
			void *tbuf1;
			if( *short_ptr1 == ((unsigned short)-1) ) {
				continue;
			}
			if ( disk_read(((unsigned short)FBLK_BASE) + *short_ptr1,tbuf1) ) {
				return -1;
			}
			else {
				unsigned short* short_ptr2 = (unsigned short*) tbuf1;
				for(int j = 0;j < PTR_MAX_PBLK;j++) {
					void *tbuf2;
					if( *short_ptr2 == ((unsigned short)-1) ) {
						continue;
					}
					if( disk_read(((unsigned short)FBLK_BASE) + *short_ptr2,tbuf2)) {
						return -1;
					}
					else {
						struct DirPair* dp = (struct DirPair*) tbuf2;	
						for(int k = 0;k < DIR_PAIR_PBLK;k++) {
							if(dp->inode_num != -1) {
								if(strcmp(name,dp->name) == 0) {
									return dp->inode_num;
								} 
							}
							dp++;
						}								
					}
					short_ptr2++;
				}
			}
			short_ptr1++;
		}
	}
	return -2;
}


*/