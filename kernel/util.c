/*
    FUSE: Filesystem in Userspace
    Copyright (C) 2001-2004  Miklos Szeredi <miklos@szeredi.hu>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include "fuse_i.h"

#include <linux/init.h>
#include <linux/slab.h>

MODULE_AUTHOR("Miklos Szeredi <miklos@szeredi.hu>");
MODULE_DESCRIPTION("Filesystem in Userspace");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

spinlock_t fuse_lock;

//初始fuse
int __init fuse_init(void)
{
	int res;

	printk(KERN_DEBUG "fuse init %s (API version %i.%i)\n",
	       FUSE_VERSION,
	       FUSE_KERNEL_VERSION, FUSE_KERNEL_MINOR_VERSION);

	spin_lock_init(&fuse_lock);
	//初始化 fuse 文件系统
	res = fuse_fs_init();
	if (res)
		goto err;
	
	res = fuse_dev_init();
	if (res)
		goto err_fs_cleanup;
	
	return 0;

  err_fs_cleanup:
	fuse_fs_cleanup();
  err:
	return res;
}

void __exit fuse_exit(void)
{
	printk(KERN_DEBUG "fuse exit\n");
	
	fuse_fs_cleanup();
	fuse_dev_cleanup();
}

module_init(fuse_init);
module_exit(fuse_exit);

/*
 * Local Variables:
 * indent-tabs-mode: t
 * c-basic-offset: 8
 * End:
 */
