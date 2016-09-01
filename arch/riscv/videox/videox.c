#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/bug.h>
#include <linux/list.h>
#include <linux/timer.h>

#define printk(...)

#define NR_FUN_UNITS 3

enum status {
    SEND_SRC_1,
    SEND_DEST_1,
    SEND_SRC_REST,
    SEND_DEST_REST,
    SENT
};

struct request {
    void* src;
    void* dest;
    size_t len;
    int opcode;
    int attr;
};

struct operation {
    struct list_head list;
    struct request req;
    struct page *src_buf[4];
    struct page *dest_buf[4];
    struct page **src;
    struct page **dest;
    int src_cnt;
    int dest_cnt;

    // Tracking issuing status
    int src_ptr;
    int dest_ptr;

    // Calculated values
    int dest_len;
    int src_last_len;
    int dest_last_len;

    int status;
};

static long videox_ioctl(struct file *, unsigned int, unsigned long);
static int videox_open(struct inode *, struct file *);
static int videox_release(struct inode *, struct file *);

static atomic_t available = ATOMIC_INIT(1);

static void* ctrl_reg;

static struct file_operations file_ops = {
    .owner = THIS_MODULE,
    .open = videox_open,
    .unlocked_ioctl = videox_ioctl,
    .release = videox_release
};

static struct miscdevice videox_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "acc_lowrisc",
    .fops = &file_ops,
};

static LIST_HEAD(pending_ops);
static LIST_HEAD(sent_ops);


/* Allocate an new pending_op struct */
static struct operation* op_new(int src_cnt, int dest_cnt) {
    struct operation* op = kzalloc(sizeof(struct operation), GFP_KERNEL);

    if (src_cnt > 4)
        op->src = kzalloc(sizeof(struct page*) * src_cnt, GFP_KERNEL);
    else
        op->src = op->src_buf;

    if (dest_cnt > 4)
        op->dest = kzalloc(sizeof(struct page*) * dest_cnt, GFP_KERNEL);
    else
        op->dest = op->dest_buf;

    op->src_cnt  = src_cnt;
    op->dest_cnt = dest_cnt;
    return op;
}

/* Free an new pending_op struct */
static void op_delete(struct operation* op) {
    kfree(op);
    if (op->src != op->src_buf) kfree(op->src);
    if (op->dest != op->dest_buf) kfree(op->dest);
}

/* Return pages to kernel */
static void op_cleanup(struct operation* op) {
    for (int i = 0; i < op->src_cnt; i++) {
        if (op->src[i]) {
            put_page(op->src[i]);
        }
    }
    for (int i = 0; i < op->dest_cnt; i++) {
        if (op->dest[i]) {
            set_page_dirty(op->dest[i]);
            put_page(op->dest[i]);
        }
    }
    printk(KERN_INFO "lowRISC videox: Released userspace memory\n");
}

static void polling_loop(void) {
    printk(KERN_INFO "lowRISC videox: Start single event loop\n");
    while (1) {
        uint32_t src_reg = ioread32(ctrl_reg + 0);
        uint32_t dest_reg = ioread32(ctrl_reg + 8);

        printk(KERN_INFO "lowRISC videox: src_reg = %d, dest_reg = %d\n", src_reg, dest_reg);

        if (dest_reg == 0 && !list_empty(&sent_ops)) {
            printk(KERN_INFO "lowRISC videox: Cleaning up finished queue...\n");
            // Pop all finished instructions
            struct operation *op, *next;
            list_for_each_entry_safe(op, next, &sent_ops, list) {
                list_del(&op->list);
                op_cleanup(op);
                op_delete(op);
            }
        }

        if (!list_empty(&pending_ops)) {
            struct operation* op = list_entry(pending_ops.next, struct operation, list);
            uint64_t inst;
            switch(op->status) {
                case SEND_SRC_1: {
                    // Make sure we can send src
                    if(src_reg == 128) return;

                    uintptr_t offset = (uintptr_t)op->req.src &~ PAGE_MASK;
                    uintptr_t length = PAGE_SIZE - offset > op->req.len ? op->req.len : PAGE_SIZE - offset;
                    inst = (page_to_phys(op->src[0]) + offset) | (length << 34) | (op->req.opcode & 7) | ((op->req.attr & 255ull) << 56);

                    // Set last flag is this is last chunk
                    if (op->src_cnt == 1) inst |= 1ull << 55;

                    printk(KERN_INFO "lowRISC videox: Issue source %016llx\n", inst);
                    iowrite32((uint32_t)inst, ctrl_reg + 0);
                    iowrite32((uint32_t)(inst >> 32), ctrl_reg + 4);

                    // Switch to SEND_DEST_1 state
                    // We send src first so data movers can feeding data
                    // And we'll send dest meanwhile
                    op->status = SEND_DEST_1;
                    op->src_ptr = 1;
                    break;
                }
                case SEND_DEST_1:
                    // Make sure we can send dest
                    if(dest_reg == 128) return;

                    uintptr_t offset = (uintptr_t)op->req.dest &~ PAGE_MASK;
                    uintptr_t length = PAGE_SIZE - offset > op->dest_len ? op->dest_len : PAGE_SIZE - offset;
                    inst = (page_to_phys(op->dest[0]) + offset) | (length << 34) | (op->req.opcode & 7) | ((op->req.attr & 255ull) << 56);
                    printk(KERN_INFO "lowRISC videox: Issue destination %016llx\n", inst);
                    iowrite32((uint32_t)inst, ctrl_reg + 8);
                    iowrite32((uint32_t)(inst >> 32), ctrl_reg + 12);
 
                    // Switch to SEND_SRC_REST state
                    // After first dest command is sent, the data mover should be able to work
                    // without blocking, then we can send rest to FIFO
                    op->dest_ptr = 1;
                    if (op->src_cnt > 1) {
                        op->status = SEND_SRC_REST;
                    } else if (op->dest_cnt > 1) {
                        op->status = SEND_DEST_REST;
                    } else {
                        op->status = SENT;
                        list_del(&op->list);
                        list_add_tail(&op->list, &sent_ops);
                    }
                    break;
                case SEND_SRC_REST:
                    // Make sure we can send src
                    if(src_reg == 128) return;

                    inst = page_to_phys(op->src[op->src_ptr++]);

                    if (op->src_cnt == op->src_ptr)
                        inst |= (uintptr_t)op->src_last_len << 34;
                    else
                        inst |= (uintptr_t)PAGE_SIZE << 34;

                    // Set last flag is this is last chunk
                    if (op->src_cnt == op->src_ptr) inst |= 1ull << 55;

                    printk(KERN_INFO "lowRISC videox: Issue source %016llx\n", inst);
                    iowrite32((uint32_t)inst, ctrl_reg + 0);
                    iowrite32((uint32_t)(inst >> 32), ctrl_reg + 4);

                    if (op->src_cnt > op->src_ptr) {
                        // Keep in current state if more blocks should be sent
                    } else if (op->dest_cnt > 1) {
                        op->status = SEND_DEST_REST;
                    } else {
                        op->status = SENT;
                        list_del(&op->list);
                        list_add_tail(&op->list, &sent_ops);
                    }
                    break;
                case SEND_DEST_REST:
                    // Make sure we can send dest
                    if(dest_reg == 128) return;

                    inst = page_to_phys(op->dest[op->dest_ptr++]);

                    if (op->dest_cnt == op->dest_ptr)
                        inst |= (uintptr_t)op->dest_last_len << 34;
                    else
                        inst |= (uintptr_t)PAGE_SIZE << 34;

                    printk(KERN_INFO "lowRISC videox: Issue destination %016llx\n", inst);
                    iowrite32((uint32_t)inst, ctrl_reg + 8);
                    iowrite32((uint32_t)(inst >> 32), ctrl_reg + 12);
 
                    if (op->dest_cnt > op->dest_ptr) {
                        // Keep in current state if more blocks should be sent
                    } else {
                        op->status = SENT;
                        list_del(&op->list);
                        list_add_tail(&op->list, &sent_ops);
                    }
                    break;
                default: BUG();
            }
            continue;
        }

        break;
    }
    printk(KERN_INFO "lowRISC videox: Finish event loop\n");
}

static size_t compute_result_len(int opcode, int attrib, size_t len) {
    if (opcode == 1) {
        len *= 2;
        if (attrib & 1) {
            opcode = 2;
            attrib >>= 1;
        }
    }
    if (opcode == 2) {
        if (attrib & 1) {
            opcode = 3;
            attrib >>= 1;
        }
    }
    if (opcode == 3) {
        len /= 2;
    }
    return len;
}

static long videox_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    printk(KERN_INFO "lowRISC videox: Received ioctl request %d with argument %lx\n", cmd, arg);
    if (cmd == 0) {
        polling_loop();

        put_user(!list_empty(&sent_ops) || !list_empty(&pending_ops), (int*)arg);
        return 0;
    } else if (cmd == 1) {
        struct request req;

        // Load operation to do from user space
        copy_from_user(&req, (void*)arg, sizeof(struct request));

        if (req.opcode < 0 || req.opcode > NR_FUN_UNITS) {
            return -EINVAL;
        }

        size_t dest_len = compute_result_len(req.opcode, req.attr, req.len);

        // Aligned access is required
        if (((uintptr_t)req.src & 63) != 0 ||
            ((uintptr_t)req.dest & 63) != 0 ||
            (req.len & 63) != 0 ||
            (dest_len & 63) != 0)
            return -EINVAL;

        // We current assume page is 4K        
        BUILD_BUG_ON((1 << PAGE_SHIFT) != SZ_4K);

        // Calculation start and end address
        uintptr_t src_start_page = ((uintptr_t)req.src & PAGE_MASK) >> PAGE_SHIFT;
        uintptr_t src_end_page = (((uintptr_t)req.src + req.len - 1) & PAGE_MASK) >> PAGE_SHIFT;
        uintptr_t src_npage = src_end_page - src_start_page + 1;

        uintptr_t dest_start_page = ((uintptr_t)req.dest & PAGE_MASK) >> PAGE_SHIFT;
        uintptr_t dest_end_page = (((uintptr_t)req.dest + dest_len - 1) & PAGE_MASK) >> PAGE_SHIFT;
        uintptr_t dest_npage = dest_end_page - dest_start_page + 1;

        // Allocate an pending_op struct
        struct operation *op = op_new(src_npage, dest_npage);

        // Fix these user pages into physical memory
        down_read(&current->mm->mmap_sem);
        int result_src = get_user_pages(src_start_page << PAGE_SHIFT, src_npage, 0, 0, op->src, NULL);
        int result_dest = get_user_pages(dest_start_page << PAGE_SHIFT, dest_npage, 1, 0, op->dest, NULL);
        up_read(&current->mm->mmap_sem);

        // If we can't map all memory, then rollback and fail
        if (result_src != src_npage || result_dest != dest_npage) {
            for (int i = 0; i < result_src; i++) put_page(op->src[i]);
            for (int i = 0; i < result_dest; i++) put_page(op->dest[i]);
            op_delete(op);
            return -EINVAL;
        }

        printk(KERN_INFO "lowRISC videox: Fix userspace memory to physical memory\n");

        op->dest_len = dest_len;
        op->src_last_len = (uintptr_t)req.src + req.len - (src_end_page << PAGE_SHIFT);
        op->dest_last_len = (uintptr_t)req.dest + dest_len - (dest_end_page << PAGE_SHIFT);

        // Now finish initialization of pending_op and add it to the queue
        op->req = req;
        list_add_tail(&op->list, &pending_ops);

        // Execute the polling loop once
        polling_loop();

        return 0;
    }

    return -ENOSYS;
}

static int videox_open(struct inode *inode, struct file *file) {
    // For accelerator, we restrict that only one device can operate on it at a time
    if (!atomic_dec_and_test(&available)) {
        atomic_inc(&available);
        return -EBUSY;
    }
    
    printk(KERN_INFO "lowRISC videox: Device opened by process %d\n", current->pid);
    return 0;
}

static int videox_release(struct inode *inode, struct file *file) {
    // Release the device so it can be used later
    atomic_inc(&available);
    printk(KERN_INFO "lowRISC videox: Device closed by process %d\n", current->pid);
	return 0;
}


static int __init videox_init(void) {
    int error = misc_register(&videox_dev);
    if (error) {
        printk(KERN_ERR "lowRISC videox: Fail to register as a misc device\n");
        return error;
    }

    ctrl_reg = ioremap_nocache(0x40012000, SZ_4K); 

    printk(KERN_INFO "lowRISC videox: Registered as an misc device\n");
    return 0;
}

static void __exit videox_exit(void) {
    misc_deregister(&videox_dev);
    printk(KERN_INFO "lowRISC videox: Deregistered\n");
}

module_init(videox_init)
module_exit(videox_exit)

MODULE_LICENSE("BSD-2");
