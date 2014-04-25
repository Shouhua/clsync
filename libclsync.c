/*
    libclsyncmgr - clsync control socket API
    
    Copyright (C) 2014  Dmitry Yu Okunev <dyokunev@ut.mephi.ru> 0x8E30679C
    
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"

#include <sys/un.h>	// for "struct sockaddr_un"

#include "socket.h"
#include "libclsync.h"
#include "malloc.h"
#include "output.h"

int printf_null(const char *const fmt, ...) {
	return 0;
}

int write_null(const char *const buf, size_t size) {
	return 0;
}

printf_funct _printf_ddd=NULL;
printf_funct _printf_dd=NULL;
printf_funct _printf_d=NULL;
printf_funct _printf_v=NULL;
printf_funct printf_e=printf_null;
write_funct _write_ddd=NULL;
write_funct _write_dd=NULL;
write_funct _write_d=NULL;
write_funct _write_v=NULL;
write_funct write_e=write_null;


int libproc_procclsyncsock(socket_sockthreaddata_t *arg, sockcmd_t *sockcmd_p) {
	clsyncproc_t		*proc_p     = arg->arg;
	clsyncsock_t		*clsyncsock_p = proc_p->sock_p;
	clsyncsock_procfunct_t   procfunct    = proc_p->procfunct;

#ifdef PARANOID
	if (procfunct == NULL) {
		printf_e("Error: libproc_procclsyncsock(): procfunct == NULL\n");
		return 0;
	}
#endif

	if(procfunct(arg, sockcmd_p))
		switch(sockcmd_p->cmd_id) {
			default:
				socket_sendinvalid(clsyncsock_p, sockcmd_p);
				break;
		}

	return 0;
}

static inline int _clsync_connect_setthreaddata(socket_sockthreaddata_t *threaddata_p, clsyncproc_t *proc_p, sockprocflags_t flags) {
	threaddata_p->procfunct		=  libproc_procclsyncsock;
	threaddata_p->clsyncsock_p	=  proc_p->sock_p;
	threaddata_p->arg		=  proc_p;
	threaddata_p->running		=  NULL;
	threaddata_p->authtype		=  SOCKAUTH_NULL;
	threaddata_p->flags		=  flags;
	threaddata_p->freefunct_arg	=  free;

	return 0;
}

static inline clsyncproc_t *_clsync_x_unix(
	const char *const socket_path, 
	clsyncsock_procfunct_t procfunct, 
	sockprocflags_t flags, 
	const char *const action, 
	clsyncsock_t *(*socket_x_unix)(const char *const)
) {
	if (procfunct == NULL) {
		errno = EINVAL;
		return NULL;
	}

	clsyncproc_t *proc_p = xmalloc(sizeof(*proc_p));
	memset(proc_p, 0, sizeof(*proc_p));

	proc_p->sock_p = socket_x_unix(socket_path);
	if(proc_p->sock_p == NULL) {
		free(proc_p);
		if(errno == EUSERS) {
			printf_e("Error: clsync_%s_unix(): Too many connections.\n", action);
			return NULL;
		}

		printf_e("Error: clsync_%s_unix(): Cannot socket_%s_unix(): %s (errno: %i)\n",
			action, action, strerror(errno), errno);
		return NULL;
	}

	socket_sockthreaddata_t *threaddata_p = socket_thread_attach(proc_p->sock_p);
	if (threaddata_p == NULL) {
		socket_cleanup(proc_p->sock_p);
		free(proc_p);
		return NULL;
	}

	_clsync_connect_setthreaddata(threaddata_p, proc_p, flags);

	proc_p->procfunct = procfunct;
	socket_thread_start(threaddata_p);

	return proc_p;
}

clsyncproc_t *clsync_listen_unix(const char *const socket_path, clsyncsock_procfunct_t procfunct, sockprocflags_t flags) {
	return _clsync_x_unix(socket_path, procfunct, flags, "listen",  socket_listen_unix);
}


clsyncproc_t *clsync_connect_unix(const char *const socket_path, clsyncsock_procfunct_t procfunct, sockprocflags_t flags) {
	return _clsync_x_unix(socket_path, procfunct, flags, "connect", socket_connect_unix);
}



