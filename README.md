# fslab
May 2019. ICS2's lab 

在此讨论，写下想法，记录进度。
最新的事项写在issues中。

## issue
百废俱兴，先设计块的规格

## block analysis
> 希望ljt来做可视化的说明
```
#define BLOCK_SIZE 4096
#define BLOCK_NUM 65536
#define DISK_SIZE (BLOCK_SIZE*BLOCK_NUM)
```
相应需求：
* 至少250MB的真实可用空间
* 至少支持32768个文件及目录
* 只需支持单个文件最大8MB，如果有能力可以支持更大的单文件大小（存在额外测试点）。
* 只需支持文件名最大长度为24字符，且同一目录下不存在名称相同的文件或目录。

$$
have: \\
65536 = 64 * 1024 = 64K \\
4096 B = 4 KB \\
total size = 256 MB \\
need: \\
250M \ or \ 
32768 * 8 \ MB = 256 \ MB \\
manage\ block: 0 - 6MB \\
manage\ block: super\ block+map\ block+inode\ block \\
super\ block = 1\ blk \\
free\ block= \frac{252M}{4K} = 63K \\
free\ block\ bitmap =  \frac{63K}{8*4K} = 2\ blk\\
inode\ bitmap = \frac{32768}{8*4K} = 1\ blk\\
inode\ block = 32768 *\alpha\ B = 32*\alpha\ KB = 8*\alpha\ blk\ ,\  \alpha = 56 \\
manage\ blk = 56*8+1+2+1 = 452\\
manage\ blk\ size = 452*4K = (256+196)*4K = 2M
$$


