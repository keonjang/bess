#include "../module.h"

static struct task_result
noop_run_task(struct module *m, void *arg)
{
	struct task_result ret;

	ret.packets = 0;
	ret.bits = 0;

	return ret;
}

static const struct mclass noop = {
	.name = "NoOP",
	.run_task = noop_run_task,
};

ADD_MCLASS(noop)
