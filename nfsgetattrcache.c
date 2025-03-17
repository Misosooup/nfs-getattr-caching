#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/nfs_fs.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/hash.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/string.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Robin Verstraelen");
MODULE_DESCRIPTION("NFS getattr cache for specific paths");

#define CACHE_TIMEOUT_MS 1000  // 1 second TTL
#define CLEANUP_INTERVAL_MS 5000  // Cleanup every 5 seconds
#define MAX_PATH_LEN 256

// Cache path configuration
static const char *cached_paths[] = {
    "/tmp/nfs"
};

#define NUM_CACHED_PATHS (sizeof(cached_paths) / sizeof(cached_paths[0]))

struct getattr_cache_entry {
    char path[MAX_PATH_LEN];
    struct kstat stat;
    unsigned long timestamp;
    struct list_head list;
};

static DEFINE_SPINLOCK(cache_lock);
static LIST_HEAD(cache_list);
static atomic_t cache_hits = ATOMIC_INIT(0);
static atomic_t cache_misses = ATOMIC_INIT(0);

static struct workqueue_struct *cleanup_workqueue;
static struct delayed_work cleanup_work;

// Original NFS inode operations
static const struct inode_operations *original_iops;
static struct super_block *nfs_sb;

static bool should_cache_path(const char *path)
{
    int i;
    for (i = 0; i < NUM_CACHED_PATHS; i++) {
        if (strncmp(path, cached_paths[i], strlen(cached_paths[i])) == 0) {
            return true;
        }
    }
    return false;
}

static char *get_full_path(const struct path *path, char *buf, int buflen)
{
    char *ret;
    ret = d_path(path, buf, buflen);
    if (IS_ERR(ret)) {
        return NULL;
    }
    return ret;
}

static void cleanup_cache(void)
{
    struct getattr_cache_entry *entry, *tmp;
    unsigned long now = jiffies;

    spin_lock(&cache_lock);
    list_for_each_entry_safe(entry, tmp, &cache_list, list) {
        if (now - entry->timestamp >= msecs_to_jiffies(CACHE_TIMEOUT_MS)) {
            list_del(&entry->list);
            kfree(entry);
        }
    }
    spin_unlock(&cache_lock);
}

static void cleanup_worker(struct work_struct *work)
{
    cleanup_cache();
    queue_delayed_work(cleanup_workqueue, &cleanup_work, 
                      msecs_to_jiffies(CLEANUP_INTERVAL_MS));
}

static int cached_getattr(struct user_namespace *user_ns,
                          const struct path *path, struct kstat *stat,
                          u32 request_mask, unsigned int query_flags)
{
    struct getattr_cache_entry *entry;
    unsigned long now = jiffies;
    int ret;
    char pathbuf[MAX_PATH_LEN];
    char *fullpath;

    if (!path || !path->dentry || !stat) {
        return -EINVAL;
    }

    fullpath = get_full_path(path, pathbuf, MAX_PATH_LEN);
    if (!fullpath) {
        return original_iops->getattr(user_ns, path, stat, request_mask, query_flags);
    }

    // Only cache specific paths
    if (!should_cache_path(fullpath)) {
        return original_iops->getattr(user_ns, path, stat, request_mask, query_flags);
    }

    spin_lock(&cache_lock);
    list_for_each_entry(entry, &cache_list, list) {
        if (strcmp(entry->path, fullpath) == 0 &&
            now - entry->timestamp < msecs_to_jiffies(CACHE_TIMEOUT_MS)) {
            memcpy(stat, &entry->stat, sizeof(struct kstat));
            atomic_inc(&cache_hits);
            spin_unlock(&cache_lock);
            return 0;
        }
    }
    spin_unlock(&cache_lock);

    atomic_inc(&cache_misses);

    // Call original getattr
    ret = original_iops->getattr(user_ns, path, stat, request_mask, query_flags);
    if (ret) {
        return ret;
    }

    // Add to cache
    entry = kmalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        return ret;
    }

    strncpy(entry->path, fullpath, MAX_PATH_LEN - 1);
    entry->path[MAX_PATH_LEN - 1] = '\0';
    memcpy(&entry->stat, stat, sizeof(struct kstat));
    entry->timestamp = now;
    INIT_LIST_HEAD(&entry->list);

    spin_lock(&cache_lock);
    list_add(&entry->list, &cache_list);
    spin_unlock(&cache_lock);

    printk(KERN_DEBUG "NFS cache: Added new cache entry for %s\n", fullpath);
    return ret;
}

static struct inode_operations cached_iops;

static int __init getattr_cache_init(void)
{
    struct path path;
    struct dentry *dentry;
    struct inode *inode;
    int ret, i;

    // Try to get the root NFS mount first
    ret = kern_path("/codebase", 0, &path);
    if (ret) {
        printk(KERN_ERR "NFS cache: Failed to find NFS mount point\n");
        return ret;
    }

    nfs_sb = path.dentry->d_sb;
    path_put(&path);

    if (!nfs_sb || !nfs_sb->s_root || !nfs_sb->s_root->d_inode) {
        printk(KERN_ERR "NFS cache: Invalid superblock or root inode\n");
        return -EINVAL;
    }

    // Create cleanup workqueue
    cleanup_workqueue = create_singlethread_workqueue("nfs_cache_cleanup");
    if (!cleanup_workqueue) {
        return -ENOMEM;
    }

    // Store original ops and create our cached version
    original_iops = nfs_sb->s_root->d_inode->i_op;
    if (!original_iops) {
        destroy_workqueue(cleanup_workqueue);
        return -EINVAL;
    }

    memcpy(&cached_iops, original_iops, sizeof(struct inode_operations));
    cached_iops.getattr = cached_getattr;

    // Hook into each path we want to cache
    for (i = 0; i < NUM_CACHED_PATHS; i++) {
        ret = kern_path(cached_paths[i], 0, &path);
        if (ret == 0) {
            dentry = path.dentry;
            inode = d_inode(dentry);
            if (inode) {
                inode->i_op = &cached_iops;
                printk(KERN_INFO "NFS cache: Hooked path: %s\n", cached_paths[i]);
            }
            path_put(&path);
        }
    }

    // Also hook the root
    nfs_sb->s_root->d_inode->i_op = &cached_iops;

    INIT_DELAYED_WORK(&cleanup_work, cleanup_worker);
    queue_delayed_work(cleanup_workqueue, &cleanup_work, 
                      msecs_to_jiffies(CLEANUP_INTERVAL_MS));

    printk(KERN_INFO "NFS cache: Module loaded (original iops: %px, new iops: %px)\n",
           original_iops, &cached_iops);
    return 0;
}

static void __exit getattr_cache_exit(void)
{
    struct getattr_cache_entry *entry, *tmp;

    // Cancel and flush cleanup work
    if (cleanup_workqueue) {
        cancel_delayed_work_sync(&cleanup_work);
        destroy_workqueue(cleanup_workqueue);
    }

    // Restore original inode operations
    if (nfs_sb && nfs_sb->s_root && nfs_sb->s_root->d_inode && original_iops) {
        nfs_sb->s_root->d_inode->i_op = original_iops;
    }

    // Free all cache entries
    spin_lock(&cache_lock);
    list_for_each_entry_safe(entry, tmp, &cache_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    spin_unlock(&cache_lock);

    printk(KERN_INFO "NFS cache: Module unloaded (hits: %d, misses: %d)\n",
           atomic_read(&cache_hits), atomic_read(&cache_misses));
}

module_init(getattr_cache_init);
module_exit(getattr_cache_exit); 
