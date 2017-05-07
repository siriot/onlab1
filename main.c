/*
 * main.c
 *
 *  Created on: 2017. ápr. 11.
 *      Author: tibi
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <asm-generic/current.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <asm-generic/uaccess.h>
#include <linux/device.h>
#include <linux/of.h>

#include <linux/fpga/fpga-mgr.h>
#include <linux/of.h>

#include <linux/fs.h>
#include <linux/miscdevice.h>

#include <asm/ioctl.h>

#include <linux/platform_device.h>

#define SLOT_FREE 0
#define SLOT_USED 1

struct dev_priv
{
	int acc_id;
	struct device *dev;
	struct list_head waiters;
	struct list_head active_list;
};

struct slot_info_t
{
	struct task_struct *user;
	int status;
	struct dev_priv *actual_dev;
};

#define EVENT_REQUEST 	1
#define EVENT_CLOSE		2
struct user_event
{
	int event_type;
	// sender process
	struct task_struct *sender;
	// wait point for event handling
	struct  completion compl;
	// list element for listing
	struct list_head waiter_list;
	// event timestamp
	struct timespec time_stamp;
	// return value, scheduler sets
	int slot_id;

	// request specific values
	int acc_id;
};

// global variables
#define SLOT_NUM 1 // TODO CORE gather it form device tree

// user interface
	static struct miscdevice misc;
	static struct file_operations dev_fops;

// event queue
	DEFINE_SPINLOCK(event_list_lock);
	LIST_HEAD(event_list);
	DECLARE_COMPLETION(event_in);
	static void add_event(struct user_event *e);

// device database
	unsigned device_num;
	struct dev_priv *device_data;

// scheduler thread
	static uint8_t quit = 0;
	DECLARE_COMPLETION(thread_stop);
	static struct task_struct *sched_thread;

//fpga manager data
	static struct fpga_manager *mgr;
// fpga slot data
	DEFINE_SPINLOCK(slot_lock);
	unsigned free_slot_num = SLOT_NUM;
	struct slot_info_t slot_info[SLOT_NUM];

static void add_event(struct user_event *e)
{
	int empty;

	spin_lock(&event_list_lock);
	empty = list_empty(&event_list);
	list_add_tail(&(e->waiter_list),&event_list);
	spin_unlock(&event_list_lock);
	// notify scheduler thread
	if(empty) complete(&event_in);
}


static int program_slot(int acc_id)
{
	static char name[100];
	snprintf(name,100,"%s.bit",device_data[acc_id].dev->of_node->name);
	pr_info("Loading configuration: %s - id :%d.\n",name,acc_id);
	return fpga_mgr_firmware_load(mgr,0,name);
}
static void hw_schedule(void)
{
	int i,j;
	struct user_event *r = NULL;
	struct dev_priv *d = NULL;

	spin_lock(&slot_lock);
	if(free_slot_num==0)
	{
		spin_unlock(&slot_lock);
		return;
	}
	for(i=0;i<SLOT_NUM;i++)
		if(slot_info[i].status==SLOT_FREE)
		{
			// try to match request with this slot
			spin_unlock(&slot_lock);
			// try actual dev
			if(slot_info[i].actual_dev && !list_empty(&(slot_info[i].actual_dev->waiters)))
			{
					r = list_entry(slot_info[i].actual_dev->waiters.next,struct user_event,waiter_list);
					d = slot_info[i].actual_dev;

					list_del(&(r->waiter_list));
					spin_lock(&slot_lock);
					slot_info[i].status = SLOT_USED;
					slot_info[i].user = r->sender;
					free_slot_num--;
					spin_unlock(&slot_lock);

					r->slot_id = i;
					pr_info("Starting process %d for slot: %d, with accel: %d.\n",r->sender->pid,r->slot_id,r->acc_id);
					complete(&(r->compl));
			}
			else
			{
				r = NULL;
				// look for other requests
				for(j=0;j<device_num;j++)
					if(!list_empty(&(device_data[j].waiters)))
					{
						r = list_entry(device_data[j].waiters.next,struct user_event,waiter_list);
						d = &device_data[j];
						break;
					}

				// serve selected request
				if(r)
				{

					list_del(&(r->waiter_list));

					// start slot programming
					if(!program_slot(r->acc_id))
					{
						// programming succeeded
						spin_lock(&slot_lock);
						slot_info[i].status = SLOT_USED;
						slot_info[i].actual_dev = d;
						slot_info[i].user = r->sender;
						free_slot_num--;
						spin_unlock(&slot_lock);

						r->slot_id = i;
					}
					else
					{
						// programming failed
						r->slot_id = -1;
					}

					// start waiter process
					pr_info("Starting process %d for slot: %d, with accel: %d.\n",r->sender->pid,r->slot_id,r->acc_id);
					complete(&(r->compl));
				}
			}
			spin_lock(&slot_lock);
		}
	spin_unlock(&slot_lock);
}

static int sched_thread_fn(void *data)
{
	pr_info("Scheduler thread started.\n");

	//wait for incoming events
	wait_for_completion_interruptible(&event_in);

	while(!quit)
	{
		struct user_event *r = NULL;
		int acc_id;
		int i;

		spin_lock(&event_list_lock);
		while(!list_empty(&event_list))
			{
				r = list_entry(event_list.next,struct user_event, waiter_list);
				list_del_init(&(r->waiter_list));

				spin_unlock(&event_list_lock);
				pr_info("Processing user event: type: %d, accel_num: %d.\n",r->event_type,r->acc_id);

				switch(r->event_type)
				{
				case EVENT_REQUEST:
					// TODO SAFETY deadlock check
					acc_id = r->acc_id;
					list_add_tail(&(r->waiter_list),&(device_data[acc_id].waiters));
					break;
				case EVENT_CLOSE:
					// freeing up used slots
					spin_lock(&slot_lock);
					for(i=0;i<SLOT_NUM;i++)
					{
						if(slot_info[i].user == r->sender)
						{
							slot_info[i].status = SLOT_FREE;
							slot_info[i].user = NULL;
							free_slot_num++;
							spin_unlock(&slot_lock);
							pr_warn("Freeing accelerator in slot %d.\n",i);
							spin_lock(&slot_lock);
						}
					}
					spin_unlock(&slot_lock);
					kfree(r);
					break;
				default:
					pr_err("Unknown event type. This might cause memory leak.\n");
					break;
				}

				spin_lock(&event_list_lock);
			}
		spin_unlock(&event_list_lock);

		hw_schedule();


		//wait for incoming events
		wait_for_completion_interruptible(&event_in);
	}
	pr_info("Scheduler thread stopped.\n");
	// TODO SAFETY free waiting request structures
	complete(&thread_stop);
	return 0;
}

static int build_device_database(void);
static void free_device_database(void);

#define PROCFS_NAME "fpga_mgr"
static struct proc_dir_entry *proc_file;
ssize_t procfile_read(struct file *file, char __user *buffer, size_t bufsize, loff_t * offset);
static struct file_operations proc_fops =
{
		.owner = THIS_MODULE,
		.read = procfile_read
};


// user interface

static ssize_t dev_open(struct inode *inode, struct file *pfile)
{
	try_module_get(THIS_MODULE);
	return 0;
}

// ioctl command codes
#define IOCTL_RELEASE 	0
#define IOCTL_REQUIRE 	1
// TODO EXTRA non-blocking require
// arguments:
//	IOCTL_RELEASE: slot number
//  IOCTL_REQUIRE: accel_id
static long dev_ioctl(struct file *pfile, unsigned int cmd, unsigned long arg)
{
	switch(cmd)
	{
		case IOCTL_RELEASE:
			if(arg >= SLOT_NUM)
			{
				// invalid slot number
				pr_err("Slot id not valid: %ld.\n",arg);
				return -EPERM;
			}
			// check whether process owns the slot
			spin_lock(&slot_lock);
			if(slot_info[arg].user != current)
			{
				int user_pid = slot_info[arg].user ? slot_info[arg].user->pid : -1;
				spin_unlock(&slot_lock);
				// other process owns the slot
				pr_err("Unauthorized slot acess. Slot user pid: %d, current pid: %d\n",user_pid,current->pid);
				return -EPERM;
			}
			slot_info[arg].user = NULL;
			slot_info[arg].status = SLOT_FREE;
			free_slot_num++;
			spin_unlock(&slot_lock);

			// notify scheduler thread
			complete(&event_in);
			return 0;
			break;
		case IOCTL_REQUIRE:
		{
			struct user_event *my_request;
			int slot_id;

			// check acc_id
			if(arg >= device_num)
			{
				pr_err("Requesting non existent accelerator: %ld.\n",arg);
				return -EPERM;
			}

			if(pfile->private_data != NULL)
			{
				pr_err("Private data is not empty.\n");
				return -EINVAL;
			}

			my_request = (struct user_event*)kmalloc(sizeof(struct user_event),GFP_KERNEL);
			if(!my_request)
			{
				pr_err("No memory for request allocation.\n");
				return -ENOMEM;
			}

			// initialize request
			my_request->acc_id = arg;
			my_request->event_type = EVENT_REQUEST;
			init_completion(&(my_request->compl));
			my_request->sender = current;
			my_request->slot_id = -1;
			INIT_LIST_HEAD(&(my_request->waiter_list));

			pfile->private_data = (void*)my_request;

			// append request to the event list
			add_event(my_request);
			// wait for load ready
			wait_for_completion(&(my_request->compl));
			// accel loaded
			slot_id = my_request->slot_id;
			kfree(my_request);
			return slot_id;
		}
			break;
		default:
			pr_err("Unknown ioctl command code: %u.\n",cmd);
			return -1;
			break;

	}
	return 0;
}

static int dev_close (struct inode *inode, struct file *pfile)
{
	// send event to delete all task related requests from the system
	struct user_event *e;
	e = (struct user_event*)kmalloc(sizeof(struct user_event),GFP_KERNEL);
	e->event_type = EVENT_CLOSE;
	e->sender = current;
	add_event(e);
	module_put(THIS_MODULE);
	return 0;
}

static struct file_operations dev_fops =
{
		.owner = THIS_MODULE,
		.open = dev_open,
		.release = dev_close,
		.unlocked_ioctl = dev_ioctl
};

static int fpga_sched_init(void)
{
	int ret ;
	pr_info("Starting fpga scheduler module.\n");

	// build database
	ret = build_device_database();
	if(ret)
		return ret;
	// register misc device in the system
	misc.fops = &dev_fops;
	misc.minor = MISC_DYNAMIC_MINOR;
	misc.name = "fpga_mgr";
	if(misc_register(&misc))
	{
		pr_warn("Couldn't initialize miscdevice /dev/fpga_mgr.\n");
		goto err1;
	}
	pr_info("Misc device initialized: /dev/fpga_mgr.\n");

	// create event handler kernel thread
	sched_thread = kthread_run(sched_thread_fn,NULL,"fpga_sched");
	if(IS_ERR(sched_thread))
	{
		pr_err("Scheduler thread failed to start.\n");
		goto err2;
	}



	// register procfs interface
	proc_file = proc_create(PROCFS_NAME,0444,NULL,&proc_fops);

	return 0;
	err2:
		misc_deregister(&misc);
	err1:
	return -1;
}

static void fpga_sched_exit(void)
{
	// unregister procfs interface
	proc_remove(proc_file);
	// deregister misc device
		misc_deregister(&misc);
	//send stop signal to the kernel thread
	quit = 1;
	complete(&event_in);
	wait_for_completion(&thread_stop);

	free_device_database();
	pr_info("Fpga scheduler module exited.\n");
}

// procfs interface for slot status observation

char log_buf[256];
ssize_t procfile_read(struct file *file, char __user *buffer, size_t bufsize, loff_t * offset)
{
	int i;
	int len = 0;

	spin_lock(&slot_lock);
	for(i=0;i<SLOT_NUM;i++)
	{
		int acc_num = slot_info[i].actual_dev ? slot_info[i].actual_dev->acc_id : -1;
		int pid = slot_info[i].user ? slot_info[i].user->pid : -1;
		len += sprintf(log_buf,"slot %d \t status: %d \t accel: %d\t pid: %d\n",i,slot_info[i].status,acc_num,pid);
	}
	spin_unlock(&slot_lock);


	if(*offset>=len) return 0;

	if(copy_to_user(buffer,log_buf,len))
		return -EFAULT;
	*offset += len;
	return len;
}

///////////////////////////////
//// VIRTUAL BUS
///////////////////////////////

static int fpga_virtual_bus_match(struct device *dev, struct device_driver *drv);

static struct bus_type fpga_virtual_bus_type =
{
		.name="fpga_virtual_bus",
		.match = fpga_virtual_bus_match
};

static int fpga_virtual_bus_match(struct device *dev, struct device_driver *drv)
{
	if(dev->bus == &fpga_virtual_bus_type) return 1;
	return 0;
}


// BUS DEVICE
static struct device fpga_virtual_bus;

// BUS DRIVER
static int fpga_virtual_driver_probe(struct device *dev)
{
	if(dev->bus == &fpga_virtual_bus_type) return 1;
	return 0;
}
static struct device_driver fpga_virtual_driver =
{
		.name = "fpga_virtual_driver",
		.bus = &fpga_virtual_bus_type,
		.owner = THIS_MODULE,
		.probe = fpga_virtual_driver_probe
};

// DEVICE ATTRIBUTE
static struct device *devices;
static ssize_t id_show (struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dev_priv *p = (struct dev_priv*)dev_get_drvdata(dev);
	return sprintf(buf,"%d",p->acc_id);
}
DEVICE_ATTR(id,0444,id_show,NULL);

static int build_device_database(void)
{
	struct device_node *base;
	struct device_node *acc;
	struct device_node *devcfg;
	int i;
	int ret;

	// init fpga_manager module
	// find devcfg device
	devcfg = of_find_node_by_name(NULL,"devcfg");
	if(!devcfg)
	{
		pr_err("Can't find devcfg node in device tree.\n");
		ret = -ENODEV;
		goto err_1;
	}
	mgr = of_fpga_mgr_get(devcfg);
	of_node_put(devcfg);
	if(IS_ERR(mgr))
	{
		pr_err("Cannot get fpga manager.\n");
		ret = -ENODEV;
		goto err_1;
	}


	//search  for the fpga_virtual node in the device tree
	base = of_find_node_by_name(NULL,"fpga_virtual");
	if(!base)
	{
		pr_err("FPGA_VIRTUAL node not found. Terminating...\n");
		ret = -1;
		goto err0;
	}
	// count existent accelerators

	device_num = of_get_child_count(base);
	if(device_num==0)
	{
		pr_err("No accelerators found.\n");
		ret = -ENODEV;
		goto err1;
	}
	pr_info("%d accelerators found.\n",device_num);

	// registering fpga virtual bus
	if(bus_register(&fpga_virtual_bus_type))
	{
		pr_err("Cannot register fpga_virtual bus.\n");
		ret = -ENODEV;
		goto err1;
	}

	// register fpga virtual bus driver
	if(driver_register(&fpga_virtual_driver))
	{
		pr_err("Cannot register fpga_virutal driver.\n");
		ret = -1;
		goto err2;
	}

	// allocate structures for all the dev_privs
	device_data = (struct dev_priv*)kmalloc(device_num*sizeof(struct dev_priv),GFP_KERNEL);
	if(!device_data)
	{
		ret = -ENOMEM;
		goto err4;
	}
	// allocate device_data
	devices = (struct device*)kzalloc(device_num*sizeof(struct device),GFP_KERNEL);
	if(!devices)
	{
		ret = -ENOMEM;
		goto err5;
	}

	// register fpga_virtual_bus device
	fpga_virtual_bus.of_node = base;
	dev_set_name(&fpga_virtual_bus,"fpga_virtual_bus_device");
	if(device_register(&fpga_virtual_bus))
	{
		pr_err("Cannot register fpga_virtual_bus device.\n");
		ret = -ENODEV;
		goto err6;
	}

	// associate structures
	i=0;
	for_each_child_of_node(base,acc)
	{
		devices[i].bus = &fpga_virtual_bus_type;
		devices[i].driver = &fpga_virtual_driver;
		devices[i].of_node = acc;
		of_node_get(acc);
		devices[i].parent = &fpga_virtual_bus;
		dev_set_name(&devices[i],"%s",acc->name);
		if(device_register(&devices[i]))
		{
			pr_err("Unable to register device.\n");
		}

		device_data[i].acc_id = i;
		INIT_LIST_HEAD(&(device_data[i].active_list));
		INIT_LIST_HEAD(&(device_data[i].waiters));
		device_data[i].dev = &devices[i];

		dev_set_drvdata(&devices[i],(void*)&device_data[i]);

		// add device attribute
		device_create_file(&devices[i],&dev_attr_id);
		i++;
	}
	/*
	int i;

	device_data = (struct dev_priv*)kmalloc(DEV_NUM*sizeof(struct dev_priv),GFP_KERNEL);

	for(i=0;i<DEV_NUM;i++)
	{
		device_data[i].acc_id = i;
		device_data[i].pdev = NULL;
		INIT_LIST_HEAD(&(device_data[i].active_list));
		INIT_LIST_HEAD(&(device_data[i].waiters));
	}
	device_num = DEV_NUM;
	 * */
	return 0;

	err6:
	kfree(devices);
	err5:
	kfree(device_data);
	err4:
	driver_unregister(&fpga_virtual_driver);
	err2:
	bus_unregister(&fpga_virtual_bus_type);
	err1:
	of_node_put(base);
	err0:
	fpga_mgr_put(mgr);
	err_1:
	pr_err("Error code: %d.\n",ret);
	return ret;

}

static void free_device_database(void)
{
	int i;
	/*
	// TODO SAFETY wake all waiting processes
	*/
	for(i=0;i<device_num;i++)
	{
		device_remove_file(&devices[i],&dev_attr_id);
		of_node_put(devices[i].of_node);
		device_unregister(&devices[i]);
	}
	of_node_put(fpga_virtual_bus.of_node);
	device_unregister(&fpga_virtual_bus);

	driver_unregister(&fpga_virtual_driver);

	/// unregister bus type
	bus_unregister(&fpga_virtual_bus_type);

	kfree(device_data);
	kfree(devices);

	fpga_mgr_put(mgr);
}



module_init(fpga_sched_init);
module_exit(fpga_sched_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tusori Tibor");
MODULE_DESCRIPTION("FPGA scheduler");
