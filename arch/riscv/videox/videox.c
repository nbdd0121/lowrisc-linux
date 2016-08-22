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

enum status {
    INIT,
    RD_LOW_SENT,
    RD_HIGH_SENT,
    WR_LOW_SENT,
    WR_HIGH_SENT,
    OP_SENT
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
    struct page *src[1];
    struct page *dest[1];
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
static struct operation* op_new(void) {
    struct operation* op = kzalloc(sizeof(struct operation), GFP_KERNEL);
    return op;
}

/* Free an new pending_op struct */
static void op_delete(struct operation* op) {
    kfree(op);
}

/* Return pages to kernel */
static void op_cleanup(struct operation* op) {
    for (int i = 0; i < 1; i++) {
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
        uint32_t reg = ioread32(ctrl_reg);
        if (reg == 0 && !list_empty(&sent_ops)) {
            printk(KERN_INFO "lowRISC videox: Cleaning up finished queue...\n");
            // Pop all finished instructions
            struct operation *op, *next;
            list_for_each_entry_safe(op, next, &sent_ops, list) {
                list_del(&op->list);
                op_cleanup(op);
                op_delete(op);
            }
        }

        if (reg != 32 && !list_empty(&pending_ops)) {
            struct operation* op = list_entry(pending_ops.next, struct operation, list);
            uint32_t inst;
            switch(op->status) {
                case INIT:
                    inst = 2 | (uint32_t)page_to_phys(op->src[0]);
                    op->status = RD_LOW_SENT;
                    break;
                case RD_LOW_SENT:
                    inst = (uint32_t)(page_to_phys(op->src[0]) >> 32);
                    op->status = RD_HIGH_SENT;
                    break;
                case RD_HIGH_SENT:
                    inst = 3 | (uint32_t)page_to_phys(op->dest[0]);
                    op->status = WR_LOW_SENT;
                    break;
                case WR_LOW_SENT:
                    inst = (uint32_t)(page_to_phys(op->dest[0]) >> 32);
                    op->status = WR_HIGH_SENT;
                    break;
                case WR_HIGH_SENT:
                    inst = (uint32_t)(((op->req.attr & 63) << 27) | (op->req.len << 14) | op->req.opcode);
                    op->status = OP_SENT;
                    list_del(&op->list);
                    list_add_tail(&op->list, &sent_ops);
                    break;
                default: BUG();
            }
            printk(KERN_INFO "lowRISC videox: Issue instruction %08x\n", inst);
            iowrite32(inst, ctrl_reg);
            continue;
        }

        break;
    }
    printk(KERN_INFO "lowRISC videox: Finish event loop\n");
}

static long videox_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    printk(KERN_INFO "lowRISC videox: Received ioctl request %d with argument %lx\n", cmd, arg);
    if (cmd == 0) {
        polling_loop();

        put_user(!list_empty(&sent_ops) || !list_empty(&pending_ops), (int*)arg);
        return 0;
    } else if (cmd == 1) {
        if (ioread32(ctrl_reg) == 32) return -EAGAIN;

        struct request req;

        // Load operation to do from user space
        copy_from_user(&req, (void*)arg, sizeof(struct request));

        if (req.opcode < 8 || req.opcode > 8) {
            return -EINVAL;
        }

        // Aligned access is required
        if (((uintptr_t)req.src & 63) != 0 ||
            ((uintptr_t)req.dest & 63) != 0 ||
            (req.len & 63) != 0 ||
            req.len > 8128)
            return -EINVAL;

        // We current assume page is 4K        
        BUILD_BUG_ON((1 << PAGE_SHIFT) != SZ_4K);

        // Calculation start and end address
        uintptr_t src_start_page = ((uintptr_t)req.src & PAGE_MASK) >> PAGE_SHIFT;
        uintptr_t src_end_page = (((uintptr_t)req.src + req.len - 1) & PAGE_MASK) >> PAGE_SHIFT;
        uintptr_t src_npage = src_end_page - src_start_page + 1;

        uintptr_t dest_start_page = ((uintptr_t)req.dest & PAGE_MASK) >> PAGE_SHIFT;
        uintptr_t dest_end_page = (((uintptr_t)req.dest + req.len - 1) & PAGE_MASK) >> PAGE_SHIFT;
        uintptr_t dest_npage = dest_end_page - dest_start_page + 1;

        if (((uintptr_t)req.src & PAGE_MASK) != (uintptr_t)req.src) {
            printk(KERN_ERR "lowRISC videox: Without scatter-gather support, currently src must be on page aligned\n");
            return -ENOSYS;
        }

        if (((uintptr_t)req.dest & PAGE_MASK) != (uintptr_t)req.dest) {
            printk(KERN_ERR "lowRISC videox: Without scatter-gather support, currently dest must be on page aligned\n");
            return -ENOSYS;
        }

        if (req.len > 4096) {
            printk(KERN_ERR "lowRISC videox: Without scatter-gather support, currently len cannot be larger than 4K\n");
            return -ENOSYS;
        }

        // Allocate an pending_op struct
        struct operation *op = op_new();

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

    // Initialize accelerator by setting base address to 0
    iowrite32(2, ctrl_reg);
    iowrite32(0, ctrl_reg);
    iowrite32(3, ctrl_reg);
    iowrite32(0, ctrl_reg);

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
