/*
 * Instaspawn: a small per-display daemon which keeps one hidden, initialized
 * st-sx process ready.  An activated worker becomes an ordinary st process;
 * the daemon immediately starts another worker to refill the pool.
 *
 * This file is included by patch/st_include.c, not compiled separately.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef INSTASPAWN_POOL_SIZE
#define INSTASPAWN_POOL_SIZE 1
#endif

#define ISP_MAGIC 0x53544950u /* "STIP" */
#define ISP_MAX_WORKERS 4
#define ISP_BACKLOG 8
#define ISP_READ_TIMEOUT 1500
#define ISP_START_TIMEOUT 10000

enum isp_type {
	ISP_READY = 1,
	ISP_ACTIVATE,
	ISP_ACTIVATED,
	ISP_FAILED,
	ISP_SPAWN,
	ISP_STATUS,
	ISP_STOP
};

struct isp_msg {
	uint32_t magic;
	uint32_t type;
	uint32_t len;
};

struct isp_reply {
	uint32_t magic;
	int32_t status;
	uint32_t warm;
};

struct isp_worker {
	pid_t pid;
	int fd;
	int ready;
};

int instaspawn_worker_mode;
static int instaspawn_worker_fd = -1;
static volatile sig_atomic_t isp_running = 1;

static int
isp_writeall(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	ssize_t n;

	while (len) {
#ifdef MSG_NOSIGNAL
		n = send(fd, p, len, MSG_NOSIGNAL);
		if (n < 0 && errno == ENOTSOCK)
			n = write(fd, p, len);
#else
		n = write(fd, p, len);
#endif
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int
isp_readall(int fd, void *buf, size_t len)
{
	char *p = buf;
	ssize_t n;

	while (len) {
		n = read(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static unsigned long
isp_hash(const char *s)
{
	unsigned long h = 2166136261u;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 16777619u;
	}
	return h;
}

static int
isp_socket_path(char *path, size_t size)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("DISPLAY");
	char fallback[64];
	int n;

	if (!display || !*display)
		display = "default";
	if (!runtime || !*runtime) {
		snprintf(fallback, sizeof fallback, "/tmp");
		runtime = fallback;
	}
	n = snprintf(path, size, "%s/st-sx-%lu-%08lx.sock",
	             runtime, (unsigned long)getuid(), isp_hash(display) & 0xfffffffful);
	return n > 0 && (size_t)n < size ? 0 : -1;
}

static int
isp_connect(void)
{
	struct sockaddr_un addr;
	char path[sizeof addr.sun_path];
	int fd;

	if (isp_socket_path(path, sizeof path) < 0)
		return -1;
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
isp_client_request(uint32_t type, const char *cwd, struct isp_reply *reply)
{
	struct isp_msg msg;
	int fd, rc = -1;
	uint32_t len = cwd ? (uint32_t)strlen(cwd) : 0;

	if (len >= PATH_MAX)
		return -1;
	if ((fd = isp_connect()) < 0)
		return -1;
	msg.magic = ISP_MAGIC;
	msg.type = type;
	msg.len = len;
	if (isp_writeall(fd, &msg, sizeof msg) < 0)
		goto done;
	if (len && isp_writeall(fd, cwd, len) < 0)
		goto done;
	if (isp_readall(fd, reply, sizeof *reply) < 0)
		goto done;
	if (reply->magic == ISP_MAGIC && reply->status == 0)
		rc = 0;
done:
	close(fd);
	return rc;
}

int
instaspawn_request(const char *cwd)
{
	struct isp_reply reply;
	char current[PATH_MAX];

	if (!cwd || !*cwd) {
		if (!getcwd(current, sizeof current))
			return -1;
		cwd = current;
	}
	return isp_client_request(ISP_SPAWN, cwd, &reply);
}

void
instaspawn_worker_init(int fd)
{
	instaspawn_worker_fd = fd;
	instaspawn_worker_mode = 1;
}

/* Called by x.c immediately before the hidden worker maps its window. */
int
instaspawn_worker_activate(void)
{
	struct isp_msg msg;
	char cwd[PATH_MAX];

	if (instaspawn_worker_fd < 0)
		return -1;
	msg.magic = ISP_MAGIC;
	msg.type = ISP_READY;
	msg.len = 0;
	if (isp_writeall(instaspawn_worker_fd, &msg, sizeof msg) < 0)
		goto fail;
	if (isp_readall(instaspawn_worker_fd, &msg, sizeof msg) < 0)
		goto fail;
	if (msg.magic != ISP_MAGIC || msg.type != ISP_ACTIVATE ||
	    msg.len >= sizeof cwd)
		goto fail;
	if (msg.len && isp_readall(instaspawn_worker_fd, cwd, msg.len) < 0)
		goto fail;
	cwd[msg.len] = '\0';
	if (msg.len) {
		if (chdir(cwd) < 0) {
			msg.type = ISP_FAILED;
			msg.len = 0;
			isp_writeall(instaspawn_worker_fd, &msg, sizeof msg);
			goto fail;
		}
		/* Keep PWD aligned with the directory, including symlink paths. */
		if (setenv("PWD", cwd, 1) < 0) {
			msg.type = ISP_FAILED;
			msg.len = 0;
			isp_writeall(instaspawn_worker_fd, &msg, sizeof msg);
			goto fail;
		}
	}
	msg.magic = ISP_MAGIC;
	msg.type = ISP_ACTIVATED;
	msg.len = 0;
	if (isp_writeall(instaspawn_worker_fd, &msg, sizeof msg) < 0)
		goto fail;
	close(instaspawn_worker_fd);
	instaspawn_worker_fd = -1;
	instaspawn_worker_mode = 0;
	/* The daemon ignores SIGCHLD/SIGPIPE; restore normal terminal semantics. */
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	return 0;
fail:
	if (instaspawn_worker_fd >= 0)
		close(instaspawn_worker_fd);
	instaspawn_worker_fd = -1;
	return -1;
}

static int
isp_resolve_executable(const char *name, char *out, size_t outsz)
{
	const char *path, *p, *end;
	char candidate[PATH_MAX];
	size_t dlen;

	(void)outsz;
	if (!name || !*name)
		return -1;
	if (strchr(name, '/'))
		return realpath(name, out) ? 0 : -1;
	path = getenv("PATH");
	if (!path)
		path = "/bin:/usr/bin";
	for (p = path; *p; p = *end ? end + 1 : end) {
		end = strchr(p, ':');
		if (!end)
			end = p + strlen(p);
		dlen = (size_t)(end - p);
		if (!dlen)
			snprintf(candidate, sizeof candidate, "./%s", name);
		else if (dlen + strlen(name) + 2 < sizeof candidate) {
			memcpy(candidate, p, dlen);
			candidate[dlen] = '/';
			strcpy(candidate + dlen + 1, name);
		} else {
			continue;
		}
		if (access(candidate, X_OK) == 0 && realpath(candidate, out))
			return 0;
	}
	return -1;
}

static int
isp_bind_socket(const char *path)
{
	struct sockaddr_un addr;
	struct stat st;
	int fd;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
	umask(077);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		int active;
		if (errno != EADDRINUSE) {
			close(fd);
			return -1;
		}
		/* Never remove a live daemon's socket, even when startup races. */
		active = isp_connect();
		if (active >= 0) {
			close(active);
			close(fd);
			errno = EADDRINUSE;
			return -1;
		}
		if (lstat(path, &st) < 0 || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
			close(fd);
			return -1;
		}
		/* Only remove an owned stale socket inode; never unlink arbitrary files. */
		if (unlink(path) < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
			close(fd);
			return -1;
		}
	}
	if (chmod(path, 0600) < 0 || listen(fd, ISP_BACKLOG) < 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	return fd;
}

static int
isp_worker_slot(struct isp_worker workers[])
{
	int i;
	for (i = 0; i < ISP_MAX_WORKERS; i++)
		if (workers[i].fd < 0)
			return i;
	return -1;
}

static int
isp_worker_count(struct isp_worker workers[], int ready_only)
{
	int i, n = 0;
	for (i = 0; i < ISP_MAX_WORKERS; i++)
		if (workers[i].fd >= 0 && (!ready_only || workers[i].ready))
			n++;
	return n;
}

static int
isp_start_worker(const char *exe, struct isp_worker workers[])
{
	int sv[2], slot, flags;
	pid_t pid;

	if ((slot = isp_worker_slot(workers)) < 0)
		return -1;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(sv[0]);
		close(sv[1]);
		return -1;
	}
	if (pid == 0) {
		close(sv[0]);
		if (sv[1] != 3) {
			if (dup2(sv[1], 3) < 0)
				_exit(126);
			close(sv[1]);
		}
		flags = fcntl(3, F_GETFD);
		if (flags >= 0)
			fcntl(3, F_SETFD, flags & ~FD_CLOEXEC);
		execl(exe, exe, "--daemon-worker", "3", (char *)NULL);
		_exit(127);
	}
	close(sv[1]);
	workers[slot].pid = pid;
	workers[slot].fd = sv[0];
	workers[slot].ready = 0;
	return 0;
}

static void
isp_remove_worker(struct isp_worker *worker)
{
	if (worker->fd >= 0)
		close(worker->fd);
	worker->fd = -1;
	worker->pid = 0;
	worker->ready = 0;
}

static void
isp_fill_pool(const char *exe, struct isp_worker workers[])
{
	while (isp_worker_count(workers, 0) < INSTASPAWN_POOL_SIZE) {
		if (isp_start_worker(exe, workers) < 0)
			break;
	}
}

static int
isp_ready_worker(struct isp_worker workers[])
{
	int i;
	for (i = 0; i < ISP_MAX_WORKERS; i++)
		if (workers[i].fd >= 0 && workers[i].ready)
			return i;
	return -1;
}

static void
isp_signal_handler(int sig)
{
	(void)sig;
	isp_running = 0;
}

static void
isp_reply(int fd, int status, unsigned int warm)
{
	struct isp_reply reply;
	reply.magic = ISP_MAGIC;
	reply.status = status;
	reply.warm = warm;
	isp_writeall(fd, &reply, sizeof reply);
}

static int
isp_spawn_normal(const char *exe, const char *cwd)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execl(exe, exe, "-d", cwd, (char *)NULL);
		_exit(127);
	}
	return 0;
}

static void
isp_handle_client(int listener, const char *exe, struct isp_worker workers[])
{
	struct isp_msg msg;
	char cwd[PATH_MAX];
	int client, worker;
	struct pollfd pfd;

	if ((client = accept(listener, NULL, NULL)) < 0)
		return;
	pfd.fd = client;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, ISP_READ_TIMEOUT) <= 0 ||
	    isp_readall(client, &msg, sizeof msg) < 0 ||
	    msg.magic != ISP_MAGIC || msg.len >= sizeof cwd) {
		isp_reply(client, -1, 0);
		close(client);
		return;
	}
	if (msg.len && isp_readall(client, cwd, msg.len) < 0) {
		isp_reply(client, -1, 0);
		close(client);
		return;
	}
	cwd[msg.len] = '\0';

	switch (msg.type) {
	case ISP_SPAWN:
		if (!msg.len && !getcwd(cwd, sizeof cwd)) {
			isp_reply(client, -1, 0);
			break;
		}
		worker = isp_ready_worker(workers);
		if (worker >= 0) {
			struct isp_msg ack;
			msg.type = ISP_ACTIVATE;
			msg.len = (uint32_t)strlen(cwd);
			if (msg.len < PATH_MAX &&
			    isp_writeall(workers[worker].fd, &msg, sizeof msg) == 0 &&
			    isp_writeall(workers[worker].fd, cwd, msg.len) == 0 &&
			    isp_readall(workers[worker].fd, &ack, sizeof ack) == 0 &&
			    ack.magic == ISP_MAGIC && ack.type == ISP_ACTIVATED && ack.len == 0) {
				isp_remove_worker(&workers[worker]);
				isp_reply(client, 0, (unsigned int)isp_worker_count(workers, 1));
			} else {
				isp_remove_worker(&workers[worker]);
				isp_reply(client, isp_spawn_normal(exe, cwd) == 0 ? 0 : -1,
				          (unsigned int)isp_worker_count(workers, 1));
			}
		} else {
			/* Cold-path fallback: don't lose the request while the pool refills. */
			isp_reply(client, isp_spawn_normal(exe, cwd) == 0 ? 0 : -1,
			          (unsigned int)isp_worker_count(workers, 1));
		}
		isp_fill_pool(exe, workers);
		break;
	case ISP_STATUS:
		isp_reply(client, 0, (unsigned int)isp_worker_count(workers, 1));
		break;
	case ISP_STOP:
		isp_reply(client, 0, (unsigned int)isp_worker_count(workers, 1));
		isp_running = 0;
		break;
	default:
		isp_reply(client, -1, 0);
		break;
	}
	close(client);
}

static void
isp_mark_worker_ready(struct isp_worker *worker)
{
	struct isp_msg msg;
	if (isp_readall(worker->fd, &msg, sizeof msg) < 0 ||
	    msg.magic != ISP_MAGIC || msg.type != ISP_READY || msg.len != 0) {
		isp_remove_worker(worker);
		return;
	}
	worker->ready = 1;
}

static void
isp_daemon_loop(const char *exe, int statusfd)
{
	struct isp_worker workers[ISP_MAX_WORKERS];
	struct pollfd pfds[ISP_MAX_WORKERS + 1];
	int listener, i, n, ready_wait;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];

	for (i = 0; i < ISP_MAX_WORKERS; i++) {
		workers[i].pid = 0;
		workers[i].fd = -1;
		workers[i].ready = 0;
	}
	if (isp_socket_path(path, sizeof path) < 0 ||
	    (listener = isp_bind_socket(path)) < 0) {
		if (statusfd >= 0)
			isp_writeall(statusfd, &(int){-1}, sizeof(int));
		return;
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGTERM, isp_signal_handler);
	signal(SIGINT, isp_signal_handler);
	isp_running = 1;
	isp_fill_pool(exe, workers);

	/* Don't report daemon startup complete until at least one X worker is hot. */
	for (ready_wait = 0; ready_wait < ISP_START_TIMEOUT / 100; ready_wait++) {
		if (isp_worker_count(workers, 1) > 0)
			break;
		memset(pfds, 0, sizeof pfds);
		pfds[0].fd = listener;
		pfds[0].events = POLLIN;
		n = 1;
		for (i = 0; i < ISP_MAX_WORKERS; i++) {
			if (workers[i].fd < 0)
				continue;
			pfds[n].fd = workers[i].fd;
			pfds[n].events = POLLIN;
			n++;
		}
		if (poll(pfds, n, 100) > 0) {
			int j = 1;
			for (i = 0; i < ISP_MAX_WORKERS; i++) {
				if (workers[i].fd < 0)
					continue;
				if (pfds[j].revents & POLLIN)
					isp_mark_worker_ready(&workers[i]);
				else if (pfds[j].revents & (POLLERR | POLLHUP | POLLNVAL))
					isp_remove_worker(&workers[i]);
				j++;
			}
			isp_fill_pool(exe, workers);
		}
	}
	if (isp_worker_count(workers, 1) == 0) {
		int failed = -1;
		if (statusfd >= 0) {
			isp_writeall(statusfd, &failed, sizeof failed);
			close(statusfd);
		}
		for (i = 0; i < ISP_MAX_WORKERS; i++)
			isp_remove_worker(&workers[i]);
		close(listener);
		unlink(path);
		return;
	}
	if (statusfd >= 0) {
		int ok = 0;
		isp_writeall(statusfd, &ok, sizeof ok);
		close(statusfd);
	}

	while (isp_running) {
		int slotmap[ISP_MAX_WORKERS];
		memset(pfds, 0, sizeof pfds);
		pfds[0].fd = listener;
		pfds[0].events = POLLIN;
		n = 1;
		for (i = 0; i < ISP_MAX_WORKERS; i++) {
			if (workers[i].fd < 0)
				continue;
			pfds[n].fd = workers[i].fd;
			pfds[n].events = POLLIN;
			slotmap[n - 1] = i;
			n++;
		}
		if (poll(pfds, n, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfds[0].revents & POLLIN)
			isp_handle_client(listener, exe, workers);
		for (i = 1; i < n; i++) {
			int slot = slotmap[i - 1];
			if (workers[slot].fd < 0)
				continue;
			if (pfds[i].revents & POLLIN)
				isp_mark_worker_ready(&workers[slot]);
			else if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
				isp_remove_worker(&workers[slot]);
		}
		isp_fill_pool(exe, workers);
	}

	for (i = 0; i < ISP_MAX_WORKERS; i++)
		isp_remove_worker(&workers[i]);
	close(listener);
	unlink(path);
}

int
instaspawn_daemon_start(const char *program)
{
	char exe[PATH_MAX];
	struct isp_reply reply;
	int pipefd[2], status, rc;
	pid_t pid;

	if (isp_client_request(ISP_STATUS, NULL, &reply) == 0) {
		fprintf(stderr, "st-sx instaspawn daemon already running (%u warm)\n", reply.warm);
		return 0;
	}
	if (isp_resolve_executable(program, exe, sizeof exe) < 0) {
		fprintf(stderr, "st: cannot resolve executable path: %s\n", program);
		return 1;
	}
	if (pipe(pipefd) < 0) {
		perror("st: pipe");
		return 1;
	}
	pid = fork();
	if (pid < 0) {
		perror("st: fork");
		close(pipefd[0]);
		close(pipefd[1]);
		return 1;
	}
	if (pid == 0) {
		int nullfd;
		close(pipefd[0]);
		if (setsid() < 0)
			_exit(1);
		chdir("/");
		nullfd = open("/dev/null", O_RDWR);
		if (nullfd >= 0) {
			dup2(nullfd, STDIN_FILENO);
			dup2(nullfd, STDOUT_FILENO);
			dup2(nullfd, STDERR_FILENO);
			if (nullfd > STDERR_FILENO)
				close(nullfd);
		}
		isp_daemon_loop(exe, pipefd[1]);
		_exit(0);
	}
	close(pipefd[1]);
	rc = isp_readall(pipefd[0], &status, sizeof status);
	close(pipefd[0]);
	if (rc < 0 || status != 0) {
		fprintf(stderr, "st: failed to start instaspawn daemon\n");
		return 1;
	}
	fprintf(stderr, "st-sx instaspawn daemon started (%d warm terminal%s)\n",
	        INSTASPAWN_POOL_SIZE, INSTASPAWN_POOL_SIZE == 1 ? "" : "s");
	return 0;
}

int
instaspawn_ipc_main(int argc, char **argv)
{
	struct isp_reply reply;
	char cwd[PATH_MAX];
	uint32_t type;
	const char *path = NULL;

	if (argc < 1) {
		fprintf(stderr, "usage: st ipc spawn [directory] | status | stop\n");
		return 1;
	}
	if (!strcmp(argv[0], "spawn")) {
		type = ISP_SPAWN;
		if (argc > 1) {
			path = argv[1];
		} else {
			if (!getcwd(cwd, sizeof cwd)) {
				perror("st ipc: getcwd");
				return 1;
			}
			path = cwd;
		}
		if (isp_client_request(type, path, &reply) < 0) {
			fprintf(stderr, "st ipc: daemon unavailable or spawn failed\n");
			return 1;
		}
		return 0;
	}
	if (!strcmp(argv[0], "status")) {
		type = ISP_STATUS;
		if (isp_client_request(type, NULL, &reply) < 0) {
			fprintf(stderr, "st ipc: daemon is not running\n");
			return 1;
		}
		printf("instaspawn daemon running; %u warm terminal(s)\n", reply.warm);
		return 0;
	}
	if (!strcmp(argv[0], "stop")) {
		type = ISP_STOP;
		if (isp_client_request(type, NULL, &reply) < 0) {
			fprintf(stderr, "st ipc: daemon is not running\n");
			return 1;
		}
		printf("instaspawn daemon stopped\n");
		return 0;
	}
	fprintf(stderr, "usage: st ipc spawn [directory] | status | stop\n");
	return 1;
}
