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
#define DISK_SIZE (BLOCK_SIZE*BLOCK_NUM)

#define DIRMODE S_IFDIR|0755
#define REGMODE S_IFREG|0644

//support function and macro

//macro constant 

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
//root directory's inode offset, \
In most U NIX file systems, the root inode number is 2.
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

//In pointer block,
//"pointer" per blk, 初始化全是-1（即无内容 65536），pointer基于FBLK_BASE，所以不会有65536个
#define PTR_MAX_PBLK	2048

//macro function

//support struct
typedef struct {
	mode_t mode;
    off_t size;
    __time_t atime;
    __time_t mtime;
    __time_t ctime;
    unsigned char pointer_bmap;
    unsigned short dir_pointer[12];
    unsigned short ind_pointer[2];
    unsigned short doub_ind_pointer;    
}Inode;
//
typedef struct {
	int inode_num; // == -1 by default (if not a name)
	char name[NAME_MAX_LEN+1];
}DirPair;

//Format the virtual block device in the following function
int mkfs()
{

	return 0;
}




// get inode num from path
// return value:
// (-1) - disk read error
// (-2) - filename or path not exist
// (-3) - root dir not included
// else return num ///////////////////////////的确改成这个会更好
int get_inode(const char* path,struct Inode *target) {
	
	// 不包含根目录，返回-3
	if (strchr(path, '/') == NULL) //////////////////开局就错= =， strchr找不到返回的是NULL，应该是 == NULL才return
		return -3;
	// 只含根目录
	if ((*path == '/') && (strlen(path) == 1)){ 
		if (get_inode_iblk(ROOT_I, target) == -1)
			return -1;
		return ROOT_I;
	}

	int dir_flag = 0;
	size_t path_len = strlen(path);
	// "/xxx/xxx/" or "/xxx/xxx" 都看作 "/xxx/xxx"，统一格式
	if (path[path_len-1] == '/'){
		path[path_len-1] = '\0';
		path_len--;
	}
    int i, j;
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
    if (get_inode_iblk(file_num, target) == -1)
		return -1;
	return file_num;
}

// get file inode in a dir by filename and dir num
// return -1 if something is wrong, return -2 if not found , else file inode offset，
int get_inode_idir(const char* filename, int dir_num){
    struct Inode dir_inode;
    // get dir inode (struct)
    int flag = get_inode_iblk(dir_num, &dir_inode);
    if (flag == -1)
        return -1;

    // get dir inode pointer number
    int dir_ptr_num = (dir_inode.pointer_bmap & 0xf);
    int ind_ptr_num = (dir_inode.pointer_bmap & 0x30) >> 4;
    int dual_ind_ptr_num = (dir_inode.pointer_bmap & 0x40) >> 6;

    int i, j, k;
    char buf[BLOCK_SIZE + 1];
    struct DirPair* dir_pair;
    // read dir and compare file name to each dir_pair
    for (i = 0; i < dir_ptr_num; i++){
        // ptr used or not, if not, continue
        if (dir_inode.dir_pointer[i] == (unsigned short)(-1)){
            i--;
            continue;
        }
		if (disk_read((unsigned short)FBLK_BASE + dir_inode.dir_pointer[i], buf)) //////////warning: 没有检测回来的值是否是0/1
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
        if (dir_inode.ind_pointer[i] == (unsigned short)(-1)){
            i--;
            continue;
        }
        char ptr_buf[BLOCK_SIZE];
        if (disk_read((unsigned short)FBLK_BASE + dir_inode.ind_pointer[i], ptr_buf)) //////////warning: 没有检测回来的值是否是0/1
			return -1;
		// pointer number in the indirect pointer block
        // unsigned int ptr_num = (unsigned int)((short)ptr_buf);
        for (k = 0; k < PTR_MAX_PBLK; k++){ /////////////////wrong, DIR_PAIR_PBLK should be PTR_MAX_PBLK
			unsigned short *short_ptr = (unsigned short *)ptr_buf;
            if (*(ptr_buf + k) == (unsigned short)(-1)){ //////////////wrong, should be *(short_ptr+k)
                continue;
            }
            if (disk_read((unsigned short)FBLK_BASE + *(short_ptr + k), buf)) //////////warning: 没有检测回来的值是否是0/1
				return -1;
			dir_pair = (struct DirPair *)buf;
            for (j = 0; j < DIR_PAIR_PBLK; j++, dir_pair++){
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
        if (dir_inode.doub_ind_pointer == (unsigned short)(-1)){ ////////////wrong,double indirect ptr就只有一个，这个不是数组，错了
            continue;
        }
        if (disk_read((unsigned short)FBLK_BASE + dir_inode.doub_ind_pointer , dptr_buf) )////////// wrong,doub_pointer should be doub_ind_pointer
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

// get inode(struct) by inode number (in bitmap)
// return -1 if something is wrong, else 0
int get_inode_iblk(int inode_num, struct Inode* inode ){
    // calculate block id and offset within a block where inode locates
    int blk_id = INODE_BASE + inode_num/INODE_NUM_PBLK;
    int blk_offset = INODE_SIZE * (inode_num % INODE_NUM_PBLK);

    char buf[BLOCK_SIZE+1];
    // get block by id and offset, write to buf
    int flag = disk_read(blk_id, buf);
    if (flag != 1){ //这里也有问题，他是正常返回0，错误返回1
		// $$
		struct Inode *inode_buf = (struct Inode*)(buf + blk_offset)
        *(inode) = *(inode_buf);
        return 0;
    }
    else
        return -1;
}






//Filesystem operations that you need to implement
int fs_getattr (const char *path, struct stat *attr)
{
    if(NULL == path) {
        printf("path is NULL\n");
        return -ENOENT;
    }

    size_t path_len = strlen(path);

    if(path[path_len-1] == '/') {
        attr.st_mode = 
    }
    else {

    }

	printf("Getattr is called:%s\n",path);
	return 0;
}

int fs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	printf("Readdir is called:%s\n", path);
	return 0;
}

int fs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	printf("Read is called:%s\n",path);
	return 0;
}

int fs_mknod (const char *path, mode_t mode, dev_t dev)
{
	printf("Mknod is called:%s\n",path);
	return 0;
}

int fs_mkdir (const char *path, mode_t mode)
{
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
	printf("Rename is called:%s\n",path);
	return 0;
}

int fs_write (const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	printf("Write is called:%s\n",path);
	return 0;
}

int fs_truncate (const char *path, off_t size)
{
	printf("Truncate is called:%s\n",path);
	return 0;
}

int fs_utime (const char *path, struct utimbuf *buffer)
{
	printf("Utime is called:%s\n",path);
	return 0;
}

int fs_statfs (const char *path, struct statvfs *stat)
{
	printf("Statfs is called:%s\n",path);
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