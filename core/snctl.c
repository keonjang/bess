#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#include <pthread.h>

#include <sys/time.h>
#include <sys/types.h>

#include "worker.h"
#include "master.h"
#include "snobj.h"
#include "module.h"
#include "port.h"
#include "time.h"

struct handler_map {
	const char *cmd;
	int pause_needed;	/* should all workers have been paused? */
	struct snobj *(*func)(struct snobj *);
};

static struct snobj *handle_reset_modules(struct snobj *);
static struct snobj *handle_reset_ports(struct snobj *);

static struct snobj *handle_reset_all(struct snobj *q)
{
	struct snobj *r;

	printf("*** reset_all requested ***\n");

	r = handle_reset_modules(NULL);
	assert(r == NULL);

	r = handle_reset_ports(NULL);
	assert(r == NULL);

	return NULL;
}

static struct snobj *handle_pause_all(struct snobj *q)
{
	pause_all_workers();
	printf("*** All workers have been paused ***\n");
	return NULL;
}

static struct snobj *handle_resume_all(struct snobj *q)
{
	resume_all_workers();
	printf("*** Resumed ***\n");
	return NULL;
}

static struct snobj *handle_add_worker(struct snobj *q)
{
	unsigned int wid;
	unsigned int core;

	struct snobj *t;

	t = snobj_eval(q, "wid");
	if (!t)
		return snobj_err(EINVAL, "Missing 'wid' field");

	wid = snobj_uint_get(t);
	if (wid >= MAX_WORKERS)
		return snobj_err(EINVAL, "'wid' must be between 0 and %d",
				MAX_WORKERS - 1);

	t = snobj_eval(q, "core");
	if (!t)
		return snobj_err(EINVAL, "Missing 'core' field");

	core = snobj_uint_get(t);
	if (core >= rte_lcore_count())
		return snobj_err(EINVAL, "'core' must be between 0 and %d",
				rte_lcore_count() - 1);

	if (is_worker_active(wid))
		return snobj_err(EEXIST, "worker:%d is already active", wid);

	launch_worker(wid, core);

	return NULL;
}

static struct snobj *handle_list_drivers(struct snobj *q)
{
	struct snobj *r;

	int cnt = 1;
	int offset;

	r = snobj_list();

	for (offset = 0; cnt != 0; offset += cnt) {
		const int arr_size = 16;
		const struct driver *drivers[arr_size];

		int i;

		cnt = list_drivers(drivers, arr_size, offset);

		for (i = 0; i < cnt; i++)
			snobj_list_add(r, snobj_str(drivers[i]->name));
	};

	return r;
}

static struct snobj *handle_reset_ports(struct snobj *q)
{
	struct port *p;

	while (list_ports((const struct port **)&p, 1, 0))
		destroy_port(p);

	printf("*** All ports have been destroyed ***\n");
	return NULL;
}

static struct snobj *handle_list_ports(struct snobj *q)
{
	struct snobj *r;

	int cnt = 1;
	int offset;

	r = snobj_list();

	for (offset = 0; cnt != 0; offset += cnt) {
		const int arr_size = 16;
		const struct port *ports[arr_size];

		int i;

		cnt = list_ports(ports, arr_size, offset);

		for (i = 0; i < cnt; i++) {
			struct snobj *port = snobj_map();

			snobj_map_set(port, "name",
					snobj_str(ports[i]->name));
			snobj_map_set(port, "driver",
					snobj_str(ports[i]->driver->name));

			snobj_list_add(r, port);
		}
	};

	return r;
}

static struct snobj *handle_create_port(struct snobj *q)
{
	const char *driver_name;
	const struct driver *driver;
	struct port *port;

	struct snobj *r;
	struct snobj *err;

	driver_name = snobj_eval_str(q, "driver");
	if (!driver_name)
		return snobj_err(EINVAL, "Missing 'driver' field");

	driver = find_driver(driver_name);
	if (!driver)
		return snobj_err(ENOENT, "No port driver '%s' found",
				driver_name);

	port = create_port(snobj_eval_str(q, "name"), driver, 
			snobj_eval(q, "arg"), &err);
	if (!port)
		return err;

	printf("Port %s created at %p\n", port->name, port);

	r = snobj_map();
	snobj_map_set(r, "name", snobj_str(port->name));

	return r;
}

static struct snobj *handle_destroy_port(struct snobj *q)
{
	const char *port_name;

	struct port *port;

	int ret;

	port_name = snobj_str_get(q);
	if (!port_name)
		return snobj_err(EINVAL, "Argument must be a name in str");
	
	port = find_port(port_name);
	if (!port)
		return snobj_err(ENOENT, "No port `%s' found", port_name);

	ret = destroy_port(port);
	if (ret)
		return snobj_errno(-ret);

	return NULL;
}

static struct snobj *handle_get_port_stats(struct snobj *q)
{
	const char *port_name;

	struct port *port;

	port_stats_t stats;

	struct snobj *r;
	struct snobj *inc;
	struct snobj *out;

	port_name = snobj_str_get(q);
	if (!port_name)
		return snobj_err(EINVAL, "Argument must be a name in str");
	
	port = find_port(port_name);
	if (!port)
		return snobj_err(ENOENT, "No port `%s' found", port_name);

	get_port_stats(port, &stats);

	inc = snobj_map();
	snobj_map_set(inc, "packets", snobj_uint(stats[PACKET_DIR_INC].packets));
	snobj_map_set(inc, "dropped", snobj_uint(stats[PACKET_DIR_INC].dropped));
	snobj_map_set(inc, "bytes",   snobj_uint(stats[PACKET_DIR_INC].bytes));

	out = snobj_map();
	snobj_map_set(out, "packets", snobj_uint(stats[PACKET_DIR_OUT].packets));
	snobj_map_set(out, "dropped", snobj_uint(stats[PACKET_DIR_OUT].dropped));
	snobj_map_set(out, "bytes",   snobj_uint(stats[PACKET_DIR_OUT].bytes));

	r = snobj_map();
	snobj_map_set(r, "inc", inc);
	snobj_map_set(r, "out", out);
	snobj_map_set(r, "timestamp", snobj_double(get_epoch_time()));

	return r;
}

static struct snobj *handle_list_mclasses(struct snobj *q)
{
	struct snobj *r;

	int cnt = 1;
	int offset;

	r = snobj_list();

	for (offset = 0; cnt != 0; offset += cnt) {
		const int arr_size = 16;
		const struct mclass *mclasses[arr_size];

		int i;
		
		cnt = list_mclasses(mclasses, arr_size, offset);

		for (i = 0; i < cnt; i++)
			snobj_list_add(r, snobj_str(mclasses[i]->name));
	};

	return r;
}

static struct snobj *handle_reset_modules(struct snobj *q)
{
	struct module *m;

	while (list_modules((const struct module **)&m, 1, 0))
		destroy_module(m);

	printf("*** All modules have been destroyed ***\n");
	return NULL;
}

static struct snobj *handle_list_modules(struct snobj *q)
{
	struct snobj *r;

	int cnt = 1;
	int offset;

	r = snobj_list();

	for (offset = 0; cnt != 0; offset += cnt) {
		const int arr_size = 16;
		const struct module *modules[arr_size];

		int i;
		
		cnt = list_modules(modules, arr_size, offset);

		for (i = 0; i < cnt; i++) {
			const struct module *m = modules[i];
			const struct mclass *mclass = m->mclass;

			struct snobj *module = snobj_map();

			snobj_map_set(module, "name", 
					snobj_str(m->name));
			snobj_map_set(module, "mclass", 
					snobj_str(mclass->name));
			if (mclass->get_desc) {
				snobj_map_set(module, "desc", 
						mclass->get_desc(m));
			}

			snobj_list_add(r, module);
		}
	};

	return r;
}

static struct snobj *handle_create_module(struct snobj *q)
{
	const char *mclass_name;
	const struct mclass *mclass;
	struct module *module;

	struct snobj *r;

	mclass_name = snobj_eval_str(q, "mclass");
	if (!mclass_name)
		return snobj_err(EINVAL, "Missing 'mclass' field");

	mclass = find_mclass(mclass_name);
	if (!mclass)
		return snobj_err(ENOENT, "No mclass '%s' found", mclass_name);

	module = create_module(snobj_eval_str(q, "name"), mclass, 
			snobj_eval(q, "arg"), &r);
	if (!module)
		return r;

	printf("Module %s created at %p\n", module->name, module);

	r = snobj_map();
	snobj_map_set(r, "name", snobj_str(module->name));

	return r;
}

static struct snobj *handle_destroy_module(struct snobj *q)
{
	const char *m_name;
	struct module *m;

	m_name = snobj_str_get(q);

	if (!m_name)
		return snobj_err(EINVAL, "Argument must be a name in str");

	if ((m = find_module(m_name)) == NULL)
		return snobj_err(ENOENT, "No module '%s' found", m_name);

	destroy_module(m);

	return NULL;
}

static struct snobj *handle_get_module_info(struct snobj *q)
{
	const char *m_name;
	struct module *m;

	struct snobj *r;
	struct snobj *gates;

	m_name = snobj_str_get(q);

	if (!m_name)
		return snobj_err(EINVAL, "Argument must be a name in str");

	if ((m = find_module(m_name)) == NULL)
		return snobj_err(ENOENT, "No module '%s' found", m_name);

	r = snobj_map();
	gates = snobj_list();

	snobj_map_set(r, "name", snobj_str(m->name));
	snobj_map_set(r, "mclass", snobj_str(m->mclass->name));

	if (m->mclass->get_desc)
		snobj_map_set(r, "desc", m->mclass->get_desc(m));

	if (m->mclass->get_dump)
		snobj_map_set(r, "dump", m->mclass->get_dump(m));

	for (int i = 0; i < m->allocated_gates; i++) {
		if (m->gates[i].m) {
			struct snobj *gate = snobj_map();
			snobj_map_set(gate, "gate", snobj_uint(i));
#if TRACK_GATES
			snobj_map_set(gate, "cnt", 
					snobj_uint(m->gates[i].cnt));
			snobj_map_set(gate, "pkts", 
					snobj_uint(m->gates[i].pkts));
			snobj_map_set(gate, "timestamp", 
					snobj_double(get_epoch_time()));
#endif
			snobj_map_set(gate, "name", snobj_str(m->gates[i].m->name));
			snobj_list_add(gates, gate);
		}
	}

	snobj_map_set(r, "gates", gates);

	return r;
}

static struct snobj *handle_connect_modules(struct snobj *q)
{
	const char *m1_name;
	const char *m2_name;
	gate_t gate;

	struct module *m1;
	struct module *m2;

	int ret;

	m1_name = snobj_eval_str(q, "m1");
	m2_name = snobj_eval_str(q, "m2");
	gate = snobj_eval_uint(q, "gate");

	if (!m1_name || !m2_name)
		return snobj_err(EINVAL, "Missing 'm1' or 'm2' field");

	if ((m1 = find_module(m1_name)) == NULL)
		return snobj_err(ENOENT, "No module '%s' found", m1_name);

	if ((m2 = find_module(m2_name)) == NULL)
		return snobj_err(ENOENT, "No module '%s' found", m2_name);

	ret = connect_modules(m1, gate, m2);
	if (ret < 0)
		return snobj_err(-ret, "Connection '%s'[%d]->'%s' failed", 
			m1_name, gate, m2_name);

	printf("%s[%d] -> %s\n", m1_name, gate, m2_name);

	return NULL;
}

static struct snobj *handle_disconnect_modules(struct snobj *q)
{
	const char *m_name;
	gate_t gate;

	struct module *m;

	int ret;

	m_name = snobj_eval_str(q, "name");
	gate = snobj_eval_uint(q, "gate");

	if (!m_name)
		return snobj_err(EINVAL, "Missing 'name' field");

	if ((m = find_module(m_name)) == NULL)
		return snobj_err(ENOENT, "No module '%s' found", m_name);

	ret = disconnect_modules(m, gate);
	if (ret < 0)
		return snobj_err(-ret, "Disconnection '%s'[%d] failed", 
			m_name, gate);

	printf("%s[%d] -> <dead end>\n", m_name, gate);

	return NULL;
}

static struct snobj *handle_attach_task(struct snobj *q)
{
	const char *m_name;
	task_id_t tid;
	int wid;		/* TODO: worker_id_t */

	struct module *m;
	struct task *t;

	m_name = snobj_eval_str(q, "name");

	if (!m_name)
		return snobj_err(EINVAL, "Missing 'name' field");

	if ((m = find_module(m_name)) == NULL)
		return snobj_err(ENOENT, "No module '%s' found", m_name);

	tid = snobj_eval_uint(q, "taskid");
	if (tid >= MAX_TASKS_PER_MODULE)
		return snobj_err(EINVAL, "'taskid' must be between 0 and %d",
				MAX_TASKS_PER_MODULE - 1);

	if ((t = m->tasks[tid]) == NULL)
		return snobj_err(ENOENT, "Task %s:%hu does not exist", 
				m_name, tid);

	if (task_is_attached(t))
		return snobj_err(EBUSY, "Task %s:%hu is already attached to "
				"a TC", m_name, tid);

	wid = snobj_eval_uint(q, "wid");
	if (wid >= MAX_WORKERS)
		return snobj_err(EINVAL, "'wid' must be between 0 and %d",
				MAX_WORKERS - 1);

	if (!is_worker_active(wid))
		return snobj_err(EINVAL, "Worker %d does not exist", wid);

	assign_default_tc(workers[wid]->s, t);

	return NULL;
}

static struct snobj *handle_enable_tcpdump(struct snobj *q)
{
	const char *m_name;
	const char *fifo;
	gate_t gate;

	struct module *m;

	int ret;

	m_name = snobj_eval_str(q, "name");
	gate = snobj_eval_uint(q, "gate");
	fifo = snobj_eval_str(q, "fifo");

	if (!m_name)
		return snobj_err(EINVAL, "Missing 'name' field");

	if ((m = find_module(m_name)) == NULL)
		return snobj_err(ENOENT, "No module '%s' found", m_name);

	if (gate >= m->allocated_gates)
		return snobj_err(EINVAL, "Gate '%hu' does not exist", gate);

	ret = enable_tcpdump(fifo, m, gate);

	if (ret < 0) {
		return snobj_err(-ret, "Enabling tcpdump %s[%d] failed",
				m_name, gate);
	}
	return NULL;
}

static struct snobj *handle_disable_tcpdump(struct snobj *q)
{
	const char *m_name;
	gate_t gate;

	struct module *m;

	int ret;

	m_name = snobj_eval_str(q, "name");
	gate = snobj_eval_uint(q, "gate");

	if (!m_name)
		return snobj_err(EINVAL, "Missing 'name' field");

	if ((m = find_module(m_name)) == NULL)
		return snobj_err(ENOENT, "No module '%s' found", m_name);

	if (gate >= m->allocated_gates)
		return snobj_err(EINVAL, "Gate '%hu' does not exist", gate);

	ret = disable_tcpdump(m, gate);

	if (ret < 0) {
		return snobj_err(-ret, "Disabling tcpdump %s[%d] failed",
				m_name, gate);
	}
	return NULL;
}

/* Adding this mostly to provide a reasonable way to exit when daemonized */
static struct snobj *handle_kill_bess(struct snobj *q)
{
	printf("bessd kill called\n");
	exit(EXIT_SUCCESS);

	/* Never called */
	return NULL;
}

static struct snobj *handle_not_implemented(struct snobj *q)
{
	return snobj_err(ENOTSUP, "Not implemented yet");
}

static struct handler_map sn_handlers[] = {
	/* remove all ports/modules/TCs/workers */
	{ "reset_all",		1, handle_reset_all },

	/* pause and resume all workers */
	{ "pause_all", 		0, handle_pause_all },
	{ "resume_all", 	0, handle_resume_all },

	{ "list_workers",	0, handle_not_implemented },
	{ "add_worker",		1, handle_add_worker },
	{ "delete_worker",	1, handle_not_implemented },

	{ "list_drivers",	0, handle_list_drivers },
	{ "import_driver",	0, handle_not_implemented },	/* TODO */

	{ "reset_ports",	1, handle_reset_ports },
	{ "list_ports",		0, handle_list_ports },
	{ "create_port", 	0, handle_create_port },
	{ "destroy_port",	0, handle_destroy_port },
	{ "get_port_stats",	0, handle_get_port_stats },

	{ "list_mclasses", 	0, handle_list_mclasses },
	{ "import_mclass",	0, handle_not_implemented },	/* TODO */

	{ "reset_modules",	1, handle_reset_modules },
	{ "list_modules",	0, handle_list_modules },
	{ "create_module", 	1, handle_create_module },
	{ "destroy_module", 	1, handle_destroy_module },
	{ "get_module_info",	0, handle_get_module_info },
	{ "connect_modules", 	1, handle_connect_modules },
	{ "disconnect_modules",	1, handle_disconnect_modules },

	{ "attach_task",	1, handle_attach_task },

	{ "enable_tcpdump",	1, handle_enable_tcpdump },
	{ "disable_tcpdump",	1, handle_disable_tcpdump },

	{ "kill_bess",		1, handle_kill_bess },

	{ NULL, 		0, NULL }
};

static struct snobj *handle_snobj_softnic(struct snobj *q)
{
	struct snobj *arg;
	const char *s;

	s = snobj_eval_str(q, "cmd");
	if (!s)
		return snobj_err(EINVAL, "Missing 'cmd' field");

	arg = snobj_map_get(q, "arg");

	for (int i = 0; sn_handlers[i].cmd != NULL; i++) {
		if (strcmp(s, sn_handlers[i].cmd) != 0)
			continue;

		if (sn_handlers[i].pause_needed && is_any_worker_running())
			return snobj_err(EBUSY, "There is a running worker");

		return sn_handlers[i].func(arg);
	}

	return snobj_err(ENOTSUP, "Unknown command in 'cmd': '%s'", s);
}

static struct snobj *handle_snobj_module(struct snobj *q)
{
	const char *m_name;
	const char *cmd;

	struct module *m;

	m_name = snobj_eval_str(q, "name");

	if (!m_name)
		return snobj_err(EINVAL, "Missing module name field 'name'");

	if ((m = find_module(m_name)) == NULL)
		return snobj_err(ENOENT, "No module '%s' found", m_name);

	cmd = snobj_eval_str(q, "cmd");

	if (strcmp(cmd, "query") == 0) {
		struct snobj *arg;

		if (!m->mclass->query)
			return snobj_err(ENOTSUP,
					"Module '%s' does not support queries",
					m_name);

		arg = snobj_eval(q, "arg");
		if (!arg) {
			struct snobj *ret;

			arg = snobj_nil();
			ret = m->mclass->query(m, arg);
			snobj_free(arg);
			return ret;
		}

		return m->mclass->query(m, arg);
	} else
		return snobj_err(ENOTSUP, "Not supported command '%s'", cmd);
}

struct snobj *handle_request(struct client *c, struct snobj *q)
{
	struct snobj *r = NULL;
	const char *s;

#if 0
	printf("Request:\n");
	snobj_dump(q);
#endif

	if (q->type != TYPE_MAP) {
		r = snobj_err(EINVAL, "The message must be a map");
		goto reply;
	}

	s = snobj_eval_str(q, "to");
	if (!s) {
		r = snobj_str("There is no 'to' field");
		goto reply;
	}

	if (strcmp(s, "softnic") == 0) {
		r = handle_snobj_softnic(q);
	} else if (strcmp(s, "module") == 0) {
		r = handle_snobj_module(q);
	} else
		r = snobj_err(EINVAL, "Unknown destination in 'to': %s", s);

reply:
	/* No response was made? (normally means "success") */
	if (!r)
		r = snobj_nil();

#if 0
	printf("Response:\n");
	snobj_dump(r);
#endif

	return r;
}
