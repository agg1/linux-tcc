#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/grinternal.h>

void
gr_log_timechange(void)
{
#ifdef CONFIG_GRKERNSEC_TIME
	if (grsec_enable_time)
		gr_log_noargs(GR_DONT_AUDIT_GOOD, GR_TIME_MSG);
#endif
	return;
}
