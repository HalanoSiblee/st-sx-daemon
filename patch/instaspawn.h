#ifndef ST_SX_INSTASPAWN_H
#define ST_SX_INSTASPAWN_H

/* Instaspawn daemon / worker interface. Included by both st.c and x.c. */
extern int instaspawn_worker_mode;

void instaspawn_worker_init(int fd);
int instaspawn_worker_activate(void);
int instaspawn_request(const char *cwd);
int instaspawn_daemon_start(const char *argv0);
int instaspawn_ipc_main(int argc, char **argv);

#endif /* ST_SX_INSTASPAWN_H */
