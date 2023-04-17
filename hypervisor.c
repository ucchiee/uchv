#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ucchiee");
MODULE_DESCRIPTION("uchv: ucchiee's hypervisor");
MODULE_VERSION("0.0.1");

int __init my_init(void)
{
  printk(KERN_INFO "Hello world.\n");
  return  0;
}

static void __exit my_exit(void)
{
  printk(KERN_INFO "Goodbye world.\n");

  return;
}

module_init(my_init);
module_exit(my_exit);
