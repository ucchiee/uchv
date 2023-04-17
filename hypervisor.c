#include <asm/asm.h>
#include <asm/cpumask.h>
#include <asm/errno.h>
#include <asm/kvm.h>
#include <asm/processor.h>
#include <linux/const.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/fs.h> /* Needed for KERN_INFO */
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kvm.h>
#include <linux/major.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ucchiee");
MODULE_DESCRIPTION("uchv: ucchiee's hypervisor");
MODULE_VERSION("0.0.1");

#define MYPAGE_SIZE 4096
#define X86_CR4_VMXE_BIT 13 // enable VMX virtualization
#define X86_CR4_VMXE _BITUL(X86_CR4_VMXE_BIT)
// #define FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX (1 << 2)
// #define FEATURE_CONTROL_LOCKED (1 << 0)
#define MSR_IA32_FEATURE_CONTROL 0x0000003a
#define EAX_EDX_VAL(val, low, high) ((low) | (high) << 32)
// #define EAX_EDX_RET(val, low, high) "=a"(low), "=d"(high)
#define MSR_IA32_VMX_CR0_FIXED0 0x00000486
#define MSR_IA32_VMX_CR0_FIXED1 0x00000487
#define MSR_IA32_VMX_CR4_FIXED0 0x00000488
#define MSR_IA32_VMX_CR4_FIXED1 0x00000489
#define MSR_IA32_VMX_BASIC 0x00000480

static inline unsigned long long notrace __rdmsr1(unsigned int msr) {
  DECLARE_ARGS(val, low, high);

  asm volatile("1: rdmsr\n"
               "2:\n" _ASM_EXTABLE_HANDLE(1b, 2b, ex_handler_rdmsr_unsafe)
               : EAX_EDX_RET(val, low, high)
               : "c"(msr));
  return EAX_EDX_VAL(val, low, high);
}

static inline uint32_t vmcs_revision_id(void) {
  return __rdmsr1(MSR_IA32_VMX_BASIC);
}

// VMXON instruction - Enter VMX operation
static inline int _vmxon(uint64_t phys) {
  uint8_t ret;

  __asm__ __volatile__("vmxon %[pa]; setna %[ret]"
                       : [ret] "=rm"(ret)
                       : [pa] "m"(phys)
                       : "cc", "memory");
  return ret;
}

bool getVmxOperation(void) {
  unsigned long cr0;
  unsigned long cr4;
  uint64_t feature_control;
  uint64_t required;
  long int vmxon_phy_region = 0;
  uint64_t *vmxon_region;
  u32 low1 = 0;

  // setting CR4.VMXE = 1 (13 bit of CR4)
  __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4) : : "memory");
  cr4 |= X86_CR4_VMXE;
  __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4) : "memory");

  // Configure IA32_FEATURE_CONTROL MSR to allow VMXON
  // Bit 0: Lock bit. If clear, VMXON causes a #GP.
  // Bit 2: Enables VMXON outside of SMX operation. If clear,
  //        VMXON outside of SMX causes a #GP
  // #GP : General Protection Exception
  required = FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX;
  required |= FEATURE_CONTROL_LOCKED;
  feature_control = __rdmsr1(MSR_IA32_FEATURE_CONTROL);

  if ((feature_control & required) != required) {
    wrmsr(MSR_IA32_FEATURE_CONTROL, feature_control | required, low1);
  }

  // Ensure bits in CR0 and CR4 are valid in VMX operation:
  // - Bit X is 1 in _FIXED0: bit X is fixed to 1 in CRx.
  // - Bit X is 0 in _FIXED1: bit X is fixed to 0 in CRx.
  __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0) : : "memory");
  cr0 &= __rdmsr1(MSR_IA32_VMX_CR0_FIXED1);
  cr0 |= __rdmsr1(MSR_IA32_VMX_CR0_FIXED0);
  __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");

  __asm__ __volatile__("mov %%cr4, $0" : "=r"(cr4) : : "memory");
  cr4 &= __rdmsr1(MSR_IA32_VMX_CR4_FIXED1);
  cr4 |= __rdmsr1(MSR_IA32_VMX_CR4_FIXED0);
  __asm__ __volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

  // allocation 4kib of memory for vmxon region
  vmxon_region = kzalloc(MYPAGE_SIZE, GFP_KERNEL);
  if (vmxon_region == NULL) {
    printk(KERN_INFO "Error allocation vmxon region\n");
    return false;
  }
  vmxon_phy_region = __pa(vmxon_region);
  *(uint32_t *)vmxon_region = vmcs_revision_id();

  if (_vmxon(vmxon_phy_region))
    return false;
  return true;
}

// looking for CPUID.1:ECX.VMX[bit 5] = 1
bool vmxSupport(void) {
  int getVmxSpport, vmxBit;
  __asm__("mov $1, $rax");
  __asm__("cpuid");
  __asm__("mov %%ecx , %0\n\t" : "=r"(getVmxSpport));
  vmxBit = (getVmxSpport >> 5) & 1;
  if (vmxBit == 1) {
    return true;
  } else {
    return false;
  }
}

int __init start_init(void) {
  if (!vmxSupport()) {
    printk(KERN_INFO "VMX support not present. Exiting.");
    return 0;
  } else {
    printk(KERN_INFO "VMX support present. Congrats.");
  }
  if (!getVmxOperation()) {
    printk(KERN_INFO "VMX Operation failed. Exiting.");
    return 0;
  } else {
    printk(KERN_INFO "VMX Operation succeeded. congrats.");
  }
  asm volatile("vmxoff\n" : : : "cc");
  return 0;
}

static void __exit end_exit(void) {
  printk(KERN_INFO "Goodbye uchv.\n");

  return;
}

module_init(start_init);
module_exit(end_exit);
