/*
    clsync - file tree sync utility based on fanotify and inotify
    
    Copyright (C) 2013  Dmitry Yu Okunev <dyokunev@ut.mephi.ru> 0x8E30679C
    
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

#include "output.h"
#include "malloc.h"
#include "socket.h"

pthread_mutex_t socket_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

int clsyncsockthreads_last	= -1;
int clsyncsockthreads_count	=  0;
int clsyncsockthreads_num	=  0;

char clsyncsockthread_busy[SOCKET_MAX+1] = {0};

socket_sockthreaddata_t sockthreaddata[SOCKET_MAX+1] = {{0}};

int socket_gc() {
	int i=clsyncsockthreads_last+1;
	while(i) {
		i--;
		switch(sockthreaddata[i].state) {
			case CLSTATE_DIED:
				printf_ddd("Debug3: socket_gc(): Forgeting clsyncsock #%u\n", i);
				pthread_join(sockthreaddata[i].thread, NULL);
				sockthreaddata[i].state = CLSTATE_NONE;
				break;
			default:
				break;
		}
	}

	return 0;
}

static char *recv_stps[SOCKET_MAX];
static char *recv_ptrs[SOCKET_MAX];

const char *const textmessage_args[SOCKCMD_MAXID] = {
	[SOCKCMD_REQUEST_NEGOTIATION] 	= "%u",
	[SOCKCMD_REPLY_NEGOTIATION] 	= "%u",
	[SOCKCMD_REPLY_ACK]		= "%03u %lu",
	[SOCKCMD_REPLY_EINVAL]		= "%03u %lu",
	[SOCKCMD_REPLY_VERSION]		= "%u %u %s",
	[SOCKCMD_REPLY_INFO]		= "%s\003/ %s\003/ %x %x",
	[SOCKCMD_REPLY_UNKNOWNCMD]	= "%03u %lu",
	[SOCKCMD_REPLY_INVALIDCMDID]	= "%lu",
};

const char *const textmessage_descr[SOCKCMD_MAXID] = {
	[SOCKCMD_REQUEST_NEGOTIATION]	= "Protocol version is %u.",
	[SOCKCMD_REPLY_NEGOTIATION]	= "Protocol version is %u.",
	[SOCKCMD_REPLY_ACK]		= "Acknowledged command: id == %03u; num == %lu.",
	[SOCKCMD_REPLY_EINVAL]		= "Rejected command: id == %03u; num == %lu. Invalid arguments: %s.",
	[SOCKCMD_REPLY_LOGIN]		= "Enter your login and password, please.",
	[SOCKCMD_REPLY_UNEXPECTEDEND]	= "Need to go, sorry. :)",
	[SOCKCMD_REPLY_DIE]		= "Okay :(",
	[SOCKCMD_REPLY_BYE]		= "Bye.",
	[SOCKCMD_REPLY_VERSION]		= "clsync v%u.%u%s",
	[SOCKCMD_REPLY_INFO]		= "config_block == \"%s\"; label == \"%s\"; flags == %x; flags_set == %x.",
	[SOCKCMD_REPLY_UNKNOWNCMD]	= "Unknown command.",
	[SOCKCMD_REPLY_INVALIDCMDID]	= "Invalid command id. Required: 0 <= cmd_id < 1000.",
};

int socket_check_bysock(int sock) {

	int error_code, ret;
	socklen_t error_code_len = sizeof(error_code);

	if((ret=getsockopt(sock, SOL_SOCKET, SO_ERROR, &error_code, &error_code_len))) {
		return errno;
	}
	if(error_code) {
		errno = error_code;
		return error_code;
	}

	return 0;
}

static inline int socket_check(clsyncsock_t *clsyncsock_p) {
	return socket_check_bysock(clsyncsock_p->sock);
}

clsyncsock_t *socket_new(int clsyncsock_sock) {
	clsyncsock_t *clsyncsock_p = xmalloc(sizeof(*clsyncsock_p));
	
	printf_dd("Debug2: socket_new(): sock == %i.\n", clsyncsock_sock);

	clsyncsock_p->sock    = clsyncsock_sock;
	
	clsyncsock_p->prot    = SOCKET_DEFAULT_PROT;
	clsyncsock_p->subprot = SOCKET_DEFAULT_SUBPROT;

	return clsyncsock_p;
}

int socket_cleanup(clsyncsock_t *clsyncsock_p) {
	int clsyncsock_sock = clsyncsock_p->sock;

	printf_dd("Debug2: socket_cleanup(): sock == %i.\n", clsyncsock_sock);

	recv_ptrs[clsyncsock_sock] = NULL;
	recv_stps[clsyncsock_sock] = NULL;

	close(clsyncsock_sock);

	free(clsyncsock_p);
	return 0;
}

int socket_thread_delete(socket_sockthreaddata_t *threaddata_p) {
	int thread_id;

	pthread_mutex_lock(&socket_thread_mutex);

	thread_id = threaddata_p->id;

	socket_cleanup(threaddata_p->clsyncsock_p);

	clsyncsockthreads_count--;

	if(clsyncsockthreads_last == thread_id)
		clsyncsockthreads_last = thread_id-1;

	clsyncsockthread_busy[thread_id]=0;

	threaddata_p->state = CLSTATE_DIED;

	if (threaddata_p->freefunct_arg != NULL)
		threaddata_p->freefunct_arg(threaddata_p->arg);

	pthread_mutex_unlock(&socket_thread_mutex);
	return 0;
}

clsyncsock_t *socket_accept(int sock) {
	// Cleaning up after died connections (getting free space for new connection)
	socket_gc();

	// Getting new connection
	int clsyncsock_sock = accept(sock, NULL, NULL);
	if(clsyncsock_sock == -1) {
		printf_e("Error: socket_accept(%i): Cannot accept(): %s (errno: %i)\n", sock, strerror(errno), errno);
		return NULL;
	}

	return socket_new(clsyncsock_sock);
}

clsyncsock_t *socket_listen_unix(const char *const socket_path) {
	int ret = 0;

	// creating a simple unix socket
	int s = -1;
	if(!ret)
		s = socket(AF_UNIX, SOCK_STREAM, 0);

	// checking the path
	if(!ret) {
		// already exists? - unlink
		if(!access(socket_path, F_OK))
			if(unlink(socket_path)) {
				printf_e("Error: Cannot unlink() \"%s\": %s (errno: %i).\n", 
					socket_path, strerror(errno), errno);
				ret = errno;
			}
	}

	// binding
	if(!ret) {
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);
		if(bind(s, (struct sockaddr *)&addr, sizeof(addr))) {
			printf_e("Error: Cannot bind() on address \"%s\": %s (errno: %i).\n",
				socket_path, strerror(errno), errno);
			ret = errno;
		}
	}

	// starting to listening
	if(!ret) {
		if(listen(s, SOCKET_BACKLOG)) {
			printf_e("Error: Cannot listen() on address \"%s\": %s (errno: %i).\n",
				socket_path, strerror(errno), errno);
			ret = errno;
		}
	}

	return socket_new(s);
}

#ifdef SOCKET_PROVIDER_LIBCLSYNC
clsyncsock_t *socket_connect_unix(const char *const socket_path) {
	return NULL;
}
#endif

int socket_send(clsyncsock_t *clsyncsock, sockcmd_id_t cmd_id, ...) {
	va_list ap;
	int ret;

	va_start(ap, cmd_id);
	char prebuf0[SOCKET_BUFSIZ], prebuf1[SOCKET_BUFSIZ], sendbuf[SOCKET_BUFSIZ];

	ret = 0;

	switch(clsyncsock->prot) {
		case 0:
			switch(clsyncsock->subprot) {
				case SUBPROT0_TEXT: {
					va_list ap_copy;

					printf_ddd("Debug3: %p %p %p\n", prebuf0, textmessage_args[cmd_id], ap_copy);

					if(textmessage_args[cmd_id]) {
						va_copy(ap_copy, ap);
						vsprintf(prebuf0, textmessage_args[cmd_id], ap_copy);
					} else
						*prebuf0 = 0;

					va_copy(ap_copy, ap);
					vsprintf(prebuf1, textmessage_descr[cmd_id], ap);

					size_t sendlen = sprintf(sendbuf, "%03u %s :%s\n", cmd_id, prebuf0, prebuf1);

					send(clsyncsock->sock, sendbuf, sendlen, 0);
					break;
				}
/*				case SUBPROT0_BINARY:
					break;*/
				default:
					printf_e("Error: socket_send(): Unknown subprotocol with id %u.\n", clsyncsock->subprot);
					ret = EINVAL;
					goto l_socket_send_end;
			}
			break;
		default:
			printf_e("Error: socket_send(): Unknown protocol with id %u.\n", clsyncsock->prot);
			ret = EINVAL;
			goto l_socket_send_end;
	}

l_socket_send_end:
	va_end(ap);
	return ret;
}

static inline int socket_overflow_fix(char *buf, char **data_start_p, char **data_end_p) {
	printf_ddd("Debug3: socket_overflow_fix(): buf==%p; data_start==%p; data_end==%p\n", buf, *data_start_p, *data_end_p);
	if(buf == *data_start_p)
		return 0;

	size_t ptr_diff = *data_start_p - buf;

	if(*data_start_p != *data_end_p) {
		*data_start_p = buf;
		*data_end_p   = buf;
		return ptr_diff;
	}

	size_t data_length = *data_end_p - *data_start_p;

	memmove(buf, *data_start_p, data_length);
	*data_start_p =  buf;
	*data_end_p   = &buf[data_length];

	return ptr_diff;
}

#define PARSE_TEXT_DATA_SSCANF(dat_t, ...) {\
	sockcmd_p->data = xmalloc(sizeof(dat_t));\
	dat_t *d = (dat_t *)sockcmd_p->data;\
	if(sscanf(args, textmessage_args[sockcmd_p->cmd_id], __VA_ARGS__) < min_args)\
		return EINVAL;\
}

static inline int parse_text_data(sockcmd_t *sockcmd_p, char *args, size_t args_len) {
	if(!args_len)
		return 0;

	int min_args = 0;
	const char *ptr = (const char *)textmessage_args[sockcmd_p->cmd_id];

	if(ptr != NULL) {
		while(*ptr) {
			if(*ptr == '%') {
				if(ptr[1] == '%')
					ptr++;
				else
					min_args++;
			}
			ptr++;
		}
	}

	switch(sockcmd_p->cmd_id) {
		case SOCKCMD_REQUEST_NEGOTIATION:
		case SOCKCMD_REPLY_NEGOTIATION:
			PARSE_TEXT_DATA_SSCANF(sockcmd_dat_negotiation_t, &d->prot, &d->subprot);
			break;
		case SOCKCMD_REPLY_ACK:
			PARSE_TEXT_DATA_SSCANF(sockcmd_dat_ack_t, &d->cmd_id, &d->cmd_num);
			break;
		case SOCKCMD_REPLY_EINVAL:
			PARSE_TEXT_DATA_SSCANF(sockcmd_dat_einval_t, &d->cmd_id, &d->cmd_num);
			break;
		case SOCKCMD_REPLY_VERSION:
			if(args_len > sizeof(1<<8))
				args[args_len=1<<8] = 0;
			PARSE_TEXT_DATA_SSCANF(sockcmd_dat_version_t, &d->major, &d->minor, &d->revision);
			break;
		case SOCKCMD_REPLY_INFO:
			if(args_len > sizeof(1<<8))
				args[args_len=1<<8] = 0;
			PARSE_TEXT_DATA_SSCANF(sockcmd_dat_info_t, &d->config_block, &d->label, &d->flags, &d->flags_set);
			break;
		case SOCKCMD_REPLY_UNKNOWNCMD:
			PARSE_TEXT_DATA_SSCANF(sockcmd_dat_unknowncmd_t, &d->cmd_id, &d->cmd_num);
			break;
		case SOCKCMD_REPLY_INVALIDCMDID:
			PARSE_TEXT_DATA_SSCANF(sockcmd_dat_invalidcmd_t, &d->cmd_num);
			break;
		default:
			sockcmd_p->data = xmalloc(args_len+1);
			memcpy(sockcmd_p->data, args, args_len);
			sockcmd_p->data[args_len] = 0;
			break;
	}

	return 0;
}

int socket_recv(clsyncsock_t *clsyncsock, sockcmd_t *sockcmd_p) {
	static char bufs[SOCKET_MAX][SOCKET_BUFSIZ];
	char *buf, *ptr, *start, *end;
	int clsyncsock_sock;
	size_t filled_length, rest_length, recv_length, filled_length_new;

	clsyncsock_sock = clsyncsock->sock;

	buf = bufs[clsyncsock_sock];

	start =  recv_stps[clsyncsock_sock];
	start = (start==NULL ? buf : start);

	ptr   =  recv_ptrs[clsyncsock_sock];
	ptr   = (ptr==NULL   ? buf : ptr);

	printf_ddd("Debug3: socket_recv(): buf==%p; start==%p; ptr==%p\n", buf, start, ptr);

	while(1) {
		filled_length = ptr-buf;
		rest_length = SOCKET_BUFSIZ-filled_length-16;

		if(rest_length <= 0) {
			if(!socket_overflow_fix(buf, &start, &ptr)) {
				printf_d("Debug: socket_recv(): Got too big message. Ignoring.\n");
				ptr = buf;
			}
			continue;
		}

		recv_length = recv(clsyncsock_sock, ptr, rest_length, 0);
		filled_length_new = filled_length + recv_length;

		if(recv_length <= 0)
			return errno;

		switch(clsyncsock->prot) {
			case 0: {
				// Checking if binary
				uint16_t cmd_id_binary = *(uint16_t *)buf;
				clsyncsock->subprot = (
								cmd_id_binary == SOCKCMD_REQUEST_NEGOTIATION ||
								cmd_id_binary == SOCKCMD_REPLY_NEGOTIATION
							) 
							? SUBPROT0_BINARY : SUBPROT0_TEXT;

				// Processing
				switch(clsyncsock->subprot) {
					case SUBPROT0_TEXT:
						if((end=strchr(ptr, '\n'))!=NULL) {
							if(sscanf(start, "%03u", (unsigned int *)&sockcmd_p->cmd_id) != 1)
								return EBADRQC;

							char *str_args = &start[3+1];
							parse_text_data(sockcmd_p, str_args, end-str_args);


							// TODO Process message here

							goto l_socket_recv_end;
						}
						break;
					default:
						return ENOPROTOOPT;
				}
				break;
			}
			default:
				return ENOPROTOOPT;
		}

		
	}

l_socket_recv_end:
	
	//       ----------------------------------
	//       buf    ptr    end    filled
	// cut:  ---------------
	//                    start    ptr
	//                     new     new

	start = &end[1];
	ptr   = &buf[filled_length_new];

	// No data buffered. Reset "start" and "ptr".

	if(start == ptr) {
		start = buf;
		ptr   = buf;
	}

	// Remembering the values

	recv_stps[clsyncsock_sock] = start;
	recv_ptrs[clsyncsock_sock] = ptr;

	printf_ddd("Debug3: socket_recv(): sockcmd_p->cmd_id == %i; buf==%p; ptr==%p; end==%p, filled=%p, buf_end==%p\n",
		sockcmd_p->cmd_id, buf, ptr, end, &buf[filled_length_new], &buf[SOCKET_BUFSIZ]);

	sockcmd_p->cmd_num++;
	return 0;
}

int socket_sendinvalid(clsyncsock_t *clsyncsock_p, sockcmd_t *sockcmd_p) {
	if(sockcmd_p->cmd_id >= 1000)
		return socket_send(clsyncsock_p, SOCKCMD_REPLY_INVALIDCMDID, sockcmd_p->cmd_num);
	else
		return socket_send(clsyncsock_p, SOCKCMD_REPLY_UNKNOWNCMD,   sockcmd_p->cmd_id, sockcmd_p->cmd_num);
}

int socket_procclsyncsock(socket_sockthreaddata_t *arg) {
	char		_sockcmd_buf[SOCKET_BUFSIZ]={0};

	sockcmd_t		*sockcmd_p    = (sockcmd_t *)_sockcmd_buf;

	clsyncsock_t		*clsyncsock_p = arg->clsyncsock_p;
	clsyncsock_procfunct_t   procfunct    = arg->procfunct;
	sockprocflags_t		 flags        = arg->flags;

	sockcmd_p->cmd_num = -1;

	enum auth_flags {
		AUTHFLAG_ENTERED_LOGIN = 0x01,
	};
	typedef enum auth_flags auth_flags_t;
	auth_flags_t	 auth_flags = 0;

	printf_ddd("Debug3: socket_procclsyncsock(): Started new thread for new connection.\n");

	arg->state = (arg->authtype == SOCKAUTH_NULL) ? CLSTATE_MAIN : CLSTATE_AUTH;
	socket_send(clsyncsock_p, SOCKCMD_REQUEST_NEGOTIATION, clsyncsock_p->prot, clsyncsock_p->subprot);

	while(*arg->running && (arg->state==CLSTATE_AUTH || arg->state==CLSTATE_MAIN)) {
		printf_ddd("Debug3: socket_procclsyncsock(): Iteration.\n");

		// Receiving message
		int ret;
		if((ret = socket_recv(clsyncsock_p, sockcmd_p))) {
			printf_e("Error: socket_procclsyncsock(): Got error while receiving a message from clsyncsock with sock %u: %s (errno: %u)\n", 
				arg->clsyncsock_p->sock, strerror(ret), ret);
			break;
		}

		if(flags&SOCKPROCFLAG_OVERRIDE_COMMON)
			goto l_socket_procclsyncsock_sw_default;

		// Processing the message
		switch(sockcmd_p->cmd_id) {
			case SOCKCMD_REPLY_NEGOTIATION:
			case SOCKCMD_REQUEST_NEGOTIATION: {
				sockcmd_dat_negotiation_t *data = (sockcmd_dat_negotiation_t *)sockcmd_p->data;
				switch(data->prot) {
					case 0:
						switch(data->subprot) {
							case SUBPROT0_TEXT:
							case SUBPROT0_BINARY:
								clsyncsock_p->subprot = data->subprot;
								if(sockcmd_p->cmd_id == SOCKCMD_REQUEST_NEGOTIATION)
									socket_send(clsyncsock_p, SOCKCMD_REPLY_NEGOTIATION, data->prot, data->subprot);
								else {
									socket_send(clsyncsock_p, SOCKCMD_REPLY_ACK,    sockcmd_p->cmd_id, sockcmd_p->cmd_num);
									printf_d("Debug2: socket_procclsyncsock(): Negotiated proto: %u %u\n", data->prot, data->subprot);
								}
								break;
							default:
								socket_send(clsyncsock_p, SOCKCMD_REPLY_EINVAL, sockcmd_p->cmd_id, sockcmd_p->cmd_num, "Incorrect subprotocol id");
						}
						break;
					default:
						socket_send(clsyncsock_p, SOCKCMD_REPLY_EINVAL, sockcmd_p->cmd_id, sockcmd_p->cmd_num, "Incorrect protocol id");
				}
				break;
			}
			case SOCKCMD_REQUEST_VERSION: {
				socket_send(clsyncsock_p, SOCKCMD_REPLY_VERSION, VERSION_MAJ, VERSION_MIN, REVISION);
				break;
			}
			case SOCKCMD_REQUEST_QUIT: {
				socket_send(clsyncsock_p, SOCKCMD_REPLY_BYE);
				arg->state = CLSTATE_DYING;
				break;
			}
			default:
l_socket_procclsyncsock_sw_default:
				if(procfunct(arg, sockcmd_p))
					socket_sendinvalid(clsyncsock_p, sockcmd_p);
				break;
		}

		if(sockcmd_p->data != NULL) {
			free(sockcmd_p->data);
			sockcmd_p->data = NULL;
		}

		// Check if the socket is still alive
		if(socket_check(clsyncsock_p)) {
			printf_d("Debug: clsyncsock socket error: %s\n", strerror(errno));
			break;
		}

		// Sending prompt
		switch(arg->state) {
			case CLSTATE_AUTH:
				if(!(auth_flags&AUTHFLAG_ENTERED_LOGIN))
					socket_send(clsyncsock_p, SOCKCMD_REQUEST_LOGIN);
				break;
			default:
				break;
		}
	}

	printf_ddd("Debug3: socket_procclsyncsock(): Ending a connection thread.\n");

	socket_thread_delete(arg);

	return 0;
}

socket_sockthreaddata_t *socket_thread_new() {
	pthread_mutex_lock(&socket_thread_mutex);
	socket_sockthreaddata_t *threaddata_p = &sockthreaddata[clsyncsockthreads_num];

	if(clsyncsockthreads_num >= SOCKET_MAX) {
		printf_e("Warning: socket_thread_new(): Too many connection threads.\n");
		errno = EUSERS;
		pthread_mutex_unlock(&socket_thread_mutex);
		return NULL;
	}

	threaddata_p->id = clsyncsockthreads_num;

	clsyncsockthread_busy[clsyncsockthreads_num]=1;
	// TODO: SECURITY: Possible DoS-attack on huge "SOCKET_MAX" value. Fix it.
	while(clsyncsockthread_busy[++clsyncsockthreads_num]);

#ifdef PARANOID
	// Processing the events: checking if previous check were been made right

	if(threaddata_p->state != CLSTATE_NONE) {
		// This's not supposed to be
		printf_e("Internal-Error: socket_newconnarg(): connproc_arg->state != CLSTATE_NONE\n");
		pthread_mutex_unlock(&socket_thread_mutex);
		return NULL;
	}
#endif

	// Processing the events: creating a thread for new connection

	printf_ddd("Debug3: socket_newconnarg(): clsyncsockthreads_count == %u;\tclsyncsockthreads_last == %u;\tclsyncsockthreads_num == %u\n", 
		clsyncsockthreads_count, clsyncsockthreads_last, clsyncsockthreads_num);

	clsyncsockthreads_last = MAX(clsyncsockthreads_last, clsyncsockthreads_num);

	clsyncsockthreads_count++;
	pthread_mutex_unlock(&socket_thread_mutex);
	return threaddata_p;
}

socket_sockthreaddata_t *socket_thread_attach(clsyncsock_t *clsyncsock_p) {

	socket_sockthreaddata_t *threaddata_p = socket_thread_new();

	if (threaddata_p == NULL)
		return NULL;

	threaddata_p->clsyncsock_p	= clsyncsock_p;

	return threaddata_p;
}

int socket_thread_start(socket_sockthreaddata_t *threaddata_p) {
	if(pthread_create(&threaddata_p->thread, NULL, (void *(*)(void *))socket_procclsyncsock, threaddata_p)) {
		printf_e("Error: socket_thread_start(): Cannot create a thread for connection: %s (errno: %i)\n", strerror(errno), errno);
		return errno;
	}

	return 0;
}

int socket_init() {
	return 0;
}

int socket_deinit() {
	return 0;
}

