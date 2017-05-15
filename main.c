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
#include <linux/uaccess.h>
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
struct virtual_dev_t;
struct slot_info_t
{
	struct virtual_dev_t *user;
	int status;
	struct dev_priv *actual_dev;
};
// Contains information about the device session.
// This structure is linked to the file descriptor and thus to the session.
#define DEV_STATUS_BLANK 		0
// user thread changes to QUEUED
#define DEV_STATUS_QUEUED 		1
/*
 * Slot connection is ready!!!
 * kthread changes to operating
 */
#define DEV_STATUS_OPERATING	2
// kthread or user thread changes state to releasing or closing
#define DEV_STATUS_RELEASING	3
#define DEV_STATUS_CLOSING		4
struct virtual_dev_t
{
	// virtual device status
	int status;
	struct spinlock status_lock;
	// Given slot
	struct slot_info_t *slot;
	// linked list for enqueueing
	struct list_head waiter_list;
	// wait point for status "operating"
	struct completion compl;
	// for debugging
	struct task_struct *user;
	// deleting flag
	int marked_for_death;
};

#define EVENT_REQUEST 	1
#define EVENT_CLOSE		2
struct user_event
{
	int event_type;
	// sender process
	struct task_struct *sender;
	// list element for listing
	struct list_head waiter_list;
	// request specific values
	int acc_id;
	// corresponding virtual device
	struct virtual_dev_t *vdev;
};


/**
 * lock policy: only kthread can get more than one spin lock, other threads may only get 1!
 */
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


static struct user_event* get_event(void)
{
	struct user_event *ue;
	spin_lock(&(event_list_lock));
	if(list_empty(&event_list))
		ue = NULL;
	else
	{
		ue = list_entry(event_list.next,struct user_event, waiter_list);
		// remove event from the queue
		list_del_init(&(ue->waiter_list));
	}
	spin_unlock(&(event_list_lock));
	return ue;
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
	struct virtual_dev_t *vdev;
	struct dev_priv *d = NULL;

	spin_lock(&slot_lock);
	if(free_slot_num==0)
	{
		spin_unlock(&slot_lock);
		return;
	}
	pr_info("Scheduling");
	for(i=0;i<SLOT_NUM;i++)
		if(slot_info[i].status==SLOT_FREE)
		{
			// try to match request with this slot
			spin_unlock(&slot_lock);
			// try actual dev
			if(slot_info[i].actual_dev && !list_empty(&(slot_info[i].actual_dev->waiters)))
			{
					vdev = list_entry(slot_info[i].actual_dev->waiters.next,struct virtual_dev_t,waiter_list);
					d = slot_info[i].actual_dev;

					list_del_init(&(vdev->waiter_list));
					spin_lock(&slot_lock);
					slot_info[i].status = SLOT_USED;
					slot_info[i].user = vdev;
					vdev->slot = &slot_info[i];
					free_slot_num--;
					spin_unlock(&slot_lock);

					spin_lock(&(vdev->status_lock));
					vdev->status = DEV_STATUS_OPERATING;
					spin_unlock(&(vdev->status_lock));
					pr_info("Starting process %d for slot: %d, with accel: %d.\n",vdev->user ? vdev->user->pid : -1,i,slot_info[i].actual_dev->acc_id);
					complete(&(vdev->compl));
			}
			else
			{
				vdev = NULL;
				// look for other requests
				for(j=0;j<device_num;j++)
					if(!list_empty(&(device_data[j].waiters)))
					{
						vdev = list_entry(device_data[j].waiters.next,struct virtual_dev_t,waiter_list);
						d = &device_data[j];
						break;
					}

				// serve selected request
				if(vdev)
				{

					list_del_init(&(vdev->waiter_list));

					// start slot programming
					if(!program_slot(d->acc_id))
					{
						// programming succeeded
						spin_lock(&slot_lock);
						slot_info[i].status = SLOT_USED;
						slot_info[i].actual_dev = d;
						slot_info[i].user = vdev;
						free_slot_num--;
						spin_unlock(&slot_lock);
						spin_lock(&(vdev->status_lock));
						vdev->status = DEV_STATUS_OPERATING;
						vdev->slot = &slot_info[i];
						spin_unlock(&(vdev->status_lock));
					}
					else
					{
						// programming failed
						spin_lock(&(vdev->status_lock));
						vdev->status = DEV_STATUS_BLANK;
						vdev->slot = NULL;
						spin_unlock(&(vdev->status_lock));
					}

					// start waiter process
					pr_info("Starting process %d for slot: %d, with accel: %d.\n",vdev->user?vdev->user->pid:-1,i,d->acc_id);
					complete(&(vdev->compl));
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
		struct virtual_dev_t *vdev = NULL;
		int acc_id;

		while((r = get_event()) != NULL)
			{
				pr_info("New event.\n");
				vdev = r->vdev;
				switch(r->event_type)
				{
					case EVENT_REQUEST:
					{
						acc_id = r->acc_id;
						// add virtual device to the waiting queue
						list_add_tail(&(vdev->waiter_list),&(device_data[acc_id].waiters));

						// free event
						kfree(r);
						r=NULL;
						pr_info("Request processed for accel: %d.\n",acc_id);
						break;
					}
					case EVENT_CLOSE:
					{
						// EVENT_CLOSE is generated when the hw file is closed
						int status;
						struct slot_info_t *slot;

						// if the status is releasing, wait for the user process thread to finish the releasing
						do
						{
							spin_lock(&(vdev->status_lock));
							status = vdev->status;
							if(status != DEV_STATUS_RELEASING) vdev->status = DEV_STATUS_CLOSING;
							spin_unlock(&(vdev->status_lock));
							if(status == DEV_STATUS_RELEASING) msleep(1);
						}while(status==DEV_STATUS_RELEASING);

						// close the virtual device according to the status
						switch(status)
						{
						case DEV_STATUS_BLANK:
							kfree(vdev);
							break;
						case DEV_STATUS_QUEUED:
							list_del(&(vdev->waiter_list));
							kfree(vdev);
							break;
						case DEV_STATUS_OPERATING:
							slot = vdev->slot;
							if(!slot) {pr_err("STATE anomaly: operating vs slot null.\n"); break;}
							// destroy slot conection
							spin_lock(&(slot_lock));
							slot->status = SLOT_FREE;
							slot->user = NULL;
							free_slot_num++;
							vdev->slot = NULL;
							spin_unlock(&(slot_lock));
							kfree(vdev);
							break;
						default:
							pr_err("Unknown vdev status: %d.\n",status);
						}

						pr_info("Virtual device closed.\n");
						break;
					}
					default:
						pr_err("Unknown event type. This might cause memory leak.\n");
						break;
				} //switch
			} //while

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
	struct virtual_dev_t *vdev;
	// Allocate new virtual device

	vdev = (struct virtual_dev_t*)kmalloc(sizeof(struct virtual_dev_t),GFP_KERNEL);
	if(!vdev)
		return -ENOMEM;
	// initialize virtual_dev
	init_completion(&(vdev->compl));
	vdev->marked_for_death = 0;
	vdev->slot= NULL;
	vdev->status = DEV_STATUS_BLANK;
	spin_lock_init(&(vdev->status_lock));
	INIT_LIST_HEAD(&(vdev->waiter_list));

	pfile->private_data = (void*)vdev;

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
	struct virtual_dev_t *vdev;
	int ok=0;

	vdev = (struct virtual_dev_t*)pfile->private_data;
	switch(cmd)
	{
		case IOCTL_RELEASE:
			// check for status
			spin_lock(&(vdev->status_lock));
			if(vdev->status == DEV_STATUS_OPERATING)
			{
				ok = 1;
				vdev->status = DEV_STATUS_RELEASING;
			}
			else
				ok = 0;
			spin_unlock(&(vdev->status_lock));
			if(!ok) return -ENODEV;

			// check whether virtual device really owns the slot
			spin_lock(&slot_lock);
			if(vdev->slot==NULL || vdev->slot->user != vdev)
			{
				spin_unlock(&slot_lock);
				// other process owns the slot
				pr_err("Unauthorized slot access.");
				return -EPERM;
			}
			// delete slot-vdev connection
			vdev->slot->status = SLOT_FREE;
			vdev->slot->user = NULL;

			vdev->slot = NULL;
			free_slot_num++;
			spin_unlock(&slot_lock);

			// change vdev status back to blank
			spin_lock(&(vdev->status_lock));
			vdev->status = DEV_STATUS_BLANK;
			spin_unlock(&(vdev->status_lock));
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

			if(pfile->private_data == NULL)
			{
				pr_err("Private data is empty.\n");
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
			my_request->sender = current;
			my_request->vdev = (struct virtual_dev_t*)pfile->private_data;
			INIT_LIST_HEAD(&(my_request->waiter_list));


			vdev->user = current;
			// check vdev status
			spin_lock(&(vdev->status_lock));
			if(vdev->status == DEV_STATUS_BLANK && vdev->marked_for_death==0)
			{
				vdev->status = DEV_STATUS_QUEUED;
				// append request to the event list
				add_event(my_request);
			}
			else
			{
				// close request already given
				spin_unlock(&(vdev->status_lock));
				kfree(my_request);
				return -EBUSY;
			}
			spin_unlock(&(vdev->status_lock));

			// wait for load ready
			wait_for_completion_killable(&(vdev->compl));
			// accel loaded
			slot_id = vdev->slot - slot_info;
			return slot_id;
		}
			break;
		default:
			pr_err("Unknown ioctl command code: %u.\n",cmd);
			return -EPERM;
			break;

	}
	return 0;
}

// give status informations
#define BUFF_LEN 256
static char buffer[BUFF_LEN];
static ssize_t dev_read (struct file *pfile, char __user *buff, size_t len, loff_t *ppos)
{
	int status;
	int slot = -1;
	int acc_id = -1;
	struct virtual_dev_t *vdev;
	int data_len;

	vdev = (struct virtual_dev_t*)pfile->private_data;
	if(!vdev) return -ENODEV;

	spin_lock(&(vdev->status_lock));
	status = vdev->status;
	if(status == DEV_STATUS_OPERATING)
	{
		slot = vdev->slot-slot_info;
		acc_id = vdev->slot->actual_dev->acc_id;
	}
	spin_unlock(&(vdev->status_lock));
	// create status report
	data_len = snprintf(buffer,BUFF_LEN,"Status :%d\nSlot: %d\nAcc_id: %d\n",status,slot,acc_id)+1;
	if(data_len > BUFF_LEN) data_len = BUFF_LEN;

	if(len > data_len) len = data_len;


	if(copy_to_user(buff,buffer,len))
		return -EFAULT;
	return len;
}

static int dev_close (struct inode *inode, struct file *pfile)
{
	struct virtual_dev_t *vdev;
	// send event to delete all task related requests from the system
	struct user_event *e;

	vdev = (struct virtual_dev_t*)pfile->private_data;

	e = (struct user_event*)kmalloc(sizeof(struct user_event),GFP_KERNEL);
	e->event_type = EVENT_CLOSE;
	e->vdev = (struct virtual_dev_t*)pfile->private_data;
	e->sender = current;

	spin_lock(&(vdev->status_lock));
	vdev->marked_for_death = 1;
	add_event(e);
	spin_unlock(&(vdev->status_lock));

	module_put(THIS_MODULE);
	return 0;
}

static struct file_operations dev_fops =
{
		.owner = THIS_MODULE,
		.open = dev_open,
		.release = dev_close,
		.unlocked_ioctl = dev_ioctl,
		.read = dev_read
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
		len += sprintf(log_buf,"slot %d \t status: %d \t accel: %d\n",i,slot_info[i].status,acc_num);
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
