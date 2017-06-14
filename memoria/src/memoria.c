/*
 ============================================================================
 Name        : memoria.c
 Authors     : Carlos Flores, Gustavo Tofaletti, Dante Romero
 Version     :
 Description : Memory Process
 ============================================================================
 */

#include <commons/bitarray.h>
#include <commons/collections/list.h>
#include <commons/collections/node.h>
#include <commons/config.h>
#include <commons/log.h>
#include <commons/string.h>
#include <shared-library/memory_prot.h>
#include <shared-library/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread_db.h>
#include "memoria.h"

#define	SOCKET_BACKLOG 100
const char * DELAY_CMD = "delay";
const char * DUMP_CACHE_CMD  = "cache";
const char * DUMP_CMD  = "dump";
const char * DUMP_MEMORY_CONTENT_CMD = "content";
const char * DUMP_MEMORY_STRUCT_CMD = "struct";
const char * FLUSH_CMD = "flush";
const char * SIZE_CMD = "size";
const char * SIZE_MEMORY_CMD = "memory";

int listenning_socket;
t_memory_conf * memory_conf;
t_log * logger;
t_log * console;

void * memory_ptr;
t_reg_invert_table * invert_table;
t_list * available_frame_list;
t_list * pages_per_process_list;

void * cache_memory;
void * last_used_time;
t_list * cache_x_process_list;

pthread_rwlock_t * memory_locks;
pthread_rwlock_t * cache_memory_locks;
pthread_mutex_t mutex_lock;

#define	LOCK_READ 				0
#define	LOCK_WRITE 				1
#define	UNLOCK 					2

#define CACHE_HIT 				1
#define CACHE_MISS 			   -1

int assign_cache_entry(int);
int assign_pages_to_process(int, int);
int get_available_frame(void);
int get_cache_entry(int, int, int);
int get_frame(int, int, int);
int inicialize_page(int, int);
int init_locks(void);
int least_recently_used(int, int *);
int read_cache_entry_and_send(int *, t_read_request *, int);
int readind_memory(int *, t_read_request *);
int reading_cache(int *,t_read_request *);
int rw_lock_unlock(pthread_rwlock_t *, int, int);
int update_cache(int, int, int, int, int, void *);
int writing_cache(t_write_request *);
int writing_memory(t_write_request *);

void assign_page(int *);
void create_logger(void);
void finalize_process(int *);
void inicialize_process(int *);
void load_memory_properties(void);
void load(void);
void mem_handshake(int *);
void memory_console(void *);
void print_memory_properties(void);
void process_request(int *);
void read_page(int *);
void write_page(int *);

int main(int argc, char * argv[]) {
	load_memory_properties();
	create_logger();
	print_memory_properties();
	load();

	// console thread
	pthread_attr_t attr;
	pthread_t thread;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&thread, &attr, &memory_console, NULL);
	pthread_attr_destroy(&attr);

	// socket thread
	int * new_sock;
	listenning_socket = open_socket(SOCKET_BACKLOG, (memory_conf->port));
	for (;;) {
		new_sock = malloc(1);
		* new_sock = accept_connection(listenning_socket);

		pthread_attr_t attr;
		pthread_t thread;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_create(&thread, &attr, &process_request, (void *) new_sock);
		pthread_attr_destroy(&attr);
	}

	close_socket(listenning_socket);
	free(memory_locks);
	free(cache_memory_locks);
	pthread_mutex_destroy(&mutex_lock);
	return EXIT_SUCCESS;
}

/**
 * @NAME load_memory_properties
 * @DESC
 */
void load_memory_properties(void) {
	t_config * conf = config_create("/home/utnso/memoria.cfg"); // TODO : Ver porque no lo toma del workspace
	memory_conf = malloc(sizeof(t_memory_conf));
	memory_conf->port = config_get_int_value(conf, "PUERTO");
	memory_conf->frames = config_get_int_value(conf, "MARCOS");
	memory_conf->frame_size = config_get_int_value(conf, "MARCO_SIZE");
	memory_conf->cache_entries = config_get_int_value(conf, "ENTRADAS_CACHE");
	memory_conf->cache_x_process = config_get_int_value(conf, "CACHE_X_PROC");
	memory_conf->memory_delay = config_get_int_value(conf, "RETARDO_MEMORIA");
	memory_conf->cache_algorithm = config_get_string_value(conf, "REEMPLAZO_CACHE");
	memory_conf->logfile = config_get_string_value(conf, "LOGFILE");
	memory_conf->consolefile = config_get_string_value(conf, "CONSOLEFILE");
}

/**
 * @NAME print_memory_properties
 * @DESC
 */
void print_memory_properties(void) {
	log_info(logger, "memory process" );
	log_info(logger, "port ---------------> %u" , (memory_conf->port));
	log_info(logger, "frames -------------> %u" , (memory_conf->frames));
	log_info(logger, "frame size ---------> %u" , (memory_conf->frame_size));
	log_info(logger, "cache entries ------> %u" , (memory_conf->cache_entries));
	log_info(logger, "cache_x_porc -------> %u" , (memory_conf->cache_x_process));
	log_info(logger, "memory delay -------> %u" , (memory_conf->memory_delay));
	log_info(logger, "cache algorithm ----> %s" , (memory_conf->cache_algorithm));
	log_info(logger, "log file -----------> %s" , (memory_conf->logfile));
	log_info(logger, "console file -------> %s" , (memory_conf->consolefile));
}

/**
 * @NAME create_logger
 * @DESC
 */
void create_logger(void) {
	logger = log_create((memory_conf->logfile), "memory_process", true, LOG_LEVEL_TRACE);
	console = log_create((memory_conf->consolefile), "memory_console", true, LOG_LEVEL_TRACE);
}

/**
 * @NAME load
 * @DESC
 */
void load(void) {
	//
	// MEMORY
	//
	memory_ptr = malloc((memory_conf->frames) * (memory_conf->frame_size));

	// available frames list
	available_frame_list = list_create();

	// invert table
	invert_table = (t_reg_invert_table *) memory_ptr;
	t_reg_invert_table * invert_table_ptr = invert_table;
	t_reg_invert_table * reg;

	int invert_table_size = sizeof(t_reg_invert_table) * (memory_conf->frames);
	int first_free_frame = (invert_table_size / (memory_conf->frame_size)) + ((((invert_table_size % (memory_conf->frame_size)) > 0) ? 1 : 0));
	int frame = 0;
	while (frame < (memory_conf->frames)) {
		reg = malloc(sizeof(t_reg_invert_table));
		reg->frame = frame;
		if (frame < first_free_frame) {
			reg->page = frame;
			reg->pid = -1;
		} else {
			list_add(available_frame_list, frame);
			reg->page = 0;
			reg->pid = 0;
		}
		memcpy(invert_table_ptr, reg, sizeof(t_reg_invert_table));
		free(reg);
		frame++;
		invert_table_ptr++;
	}

	// pages per process
	pages_per_process_list = list_create();


	//
	// CACHE MEMORY
	//
	cache_memory = malloc((memory_conf->cache_entries) * (sizeof(t_cache_memory)));
	last_used_time = malloc((memory_conf->cache_entries) * (sizeof(char **)));
	cache_x_process_list = list_create();

	t_cache_memory * cache_memory_ptr = (t_cache_memory *) cache_memory;
	char ** last_used_time_ptr = (char **) last_used_time;

	t_cache_memory * cache_memory;
	int j;
	for (j = 0; j < (memory_conf->cache_entries); j++) {
		cache_memory = malloc(sizeof(t_cache_memory));
		cache_memory->pid = -1;
		cache_memory->page = -1;
		cache_memory->content = malloc((memory_conf->frame_size));
		memcpy(cache_memory_ptr, cache_memory, sizeof(t_cache_memory));
		*last_used_time_ptr = "\0";
		free(cache_memory);

		cache_memory_ptr++;
		last_used_time_ptr++;
	}

	init_locks();

}

/**
 * @NAME init_locks
 * @DESC
 * @RETURN
 */
int init_locks(void) {
	int i;
	memory_locks = (pthread_rwlock_t *) malloc((memory_conf->frames) * sizeof(pthread_rwlock_t));
	for (i = 0 ;i < (memory_conf->frames); i++) {
		if (pthread_rwlock_init(memory_locks + i, NULL) != 0) {
			log_error(logger, "Error starting memory rw lock %d", i);
			exit(EXIT_FAILURE);
		}
	}
	cache_memory_locks = (pthread_rwlock_t *) malloc((memory_conf->cache_entries) * sizeof(pthread_rwlock_t));
	for (i = 0 ;i < (memory_conf->cache_entries); i++) {
		if (pthread_rwlock_init(cache_memory_locks + i, NULL) != 0) {
			log_error(logger, "Error starting cache memory rw lock %d", i);
			exit(EXIT_FAILURE);
		}
	}
	pthread_mutex_init(&mutex_lock, NULL);
	return EXIT_SUCCESS;
}

/**
 * @NAME rw_lock_unlock
 * @DESC
 * @PARAMS
 * @RETURN
 */
int rw_lock_unlock(pthread_rwlock_t * locks, int action, int entry) {
	switch (action) {
	case LOCK_READ :
		pthread_rwlock_rdlock(locks + entry);
		break;
	case LOCK_WRITE :
		pthread_rwlock_wrlock(locks + entry);
		break;
	case UNLOCK :
		pthread_rwlock_unlock(locks + entry);
		break;
	}
	return EXIT_SUCCESS;
}

/**
 * @NAME process_request
 * @DESC
 * @PARAMS
 */
void process_request(int * client_socket) {
	int ope_code = recv_operation_code(client_socket, logger);
	while (ope_code != DISCONNECTED_CLIENT) {
		log_info(logger, "------ client %d >> operation code : %d", * client_socket, ope_code);
		switch (ope_code) {
		case HANDSHAKE_OC:
			mem_handshake(client_socket);
			break;
		case INIT_PROCESS_OC:
			inicialize_process(client_socket);
			break;
		case READ_OC:
			read_page(client_socket);
			break;
		case WRITE_OC:
			write_page(client_socket);
			break;
		case ASSIGN_PAGE_OC:
			assign_page(client_socket);
			break;
		case END_PROCESS_OC:
			finalize_process(client_socket);
			break;
		default:;
		}
		ope_code = recv_operation_code(client_socket, logger);
	}
	close_client(* client_socket);
	free(client_socket);
	return;
}

/**
 * @NAME mem_handshake
 * @DESC
 * @PARAMS
 */
void mem_handshake(int * client_socket) {
	handshake_resp(client_socket, (memory_conf->frame_size));
}

/**
 * @NAME inicialize_process
 * @DESC
 * @PARAMS
 */
void inicialize_process(int * client_socket) {
	t_init_process_request * init_req = init_process_recv_req(client_socket, logger);
	if ((init_req->exec_code) == DISCONNECTED_CLIENT) return;
	int resp_code = assign_pages_to_process((init_req->pid), (init_req->pages));
	free(init_req);
	init_process_send_resp(client_socket, resp_code);
}

/**
 * @NAME assign_pages_to_process
 * @DESC
 * @PARAMS
 * @RETURN
 */
int assign_pages_to_process(int pid, int required_pages) {

	pthread_mutex_lock(&mutex_lock);

	if ((available_frame_list->elements_count) < required_pages) { // check available frames for request
		return ENOSPC;
	}

	int located = -1;
	t_reg_pages_process_table * pages_per_process;
	if ((pages_per_process_list->elements_count) > 0) { // searching process on pages per process list
		int index = 0;
		while (located < 0 && index < (pages_per_process_list->elements_count)) {
			pages_per_process = (t_reg_pages_process_table *) list_get(pages_per_process_list, index);
			if ((pages_per_process->pid) == pid) {
				located = 1; // process exists on list
				break;
			}
			index++;
		}
	}

	if (located < 0) {
		// process  doesn't exist on pages per process list
		// creating node for list
		pages_per_process = (t_reg_pages_process_table *) malloc (sizeof(t_reg_pages_process_table));
		pages_per_process->pid = pid;
		pages_per_process->pages_count= 0;
		list_add(pages_per_process_list, pages_per_process);
	}

	// assigning frames for process pages
	int i;
	for (i = 0; i < required_pages; i++) {
		(pages_per_process->pages_count)++;
		inicialize_page(pid, ((pages_per_process->pages_count) - 1));
	}

	pthread_mutex_unlock(&mutex_lock);

	return SUCCESS;
}

/**
 * @NAME inicialize_page
 * @DESC
 * @PARAMS
 * @RETURN
 */
int inicialize_page(int pid, int page) {
	int free_frame = get_available_frame(); // getting available frame
	t_reg_invert_table * invert_table_ptr = (t_reg_invert_table *) invert_table;
	invert_table_ptr += free_frame;
	invert_table_ptr->pid = pid;
	invert_table_ptr->page = page;
	return EXIT_SUCCESS;
}

/**
 * @NAME get_available_frame
 * @DESC
 * @RETURN
 */
int get_available_frame(void) {
	return list_remove(available_frame_list, 0);
}

/**
 * @NAME write_page
 * @DESC
 * @PARAMS
 */
void write_page(int * client_socket) {
	t_write_request * w_req = write_recv_req(client_socket, logger);
	if ((w_req->exec_code) == DISCONNECTED_CLIENT) return;
	writing_cache(w_req);
	writing_memory(w_req);
	free(w_req->buffer);
	free(w_req);
	write_send_resp(client_socket, SUCCESS);
}

/**
 * @NAME writing_cache
 * @DESC
 * @PARAMS
 * @RETURN
 */
int writing_cache(t_write_request * w_req) {
	int cache_entry_to_write = get_cache_entry((w_req->pid), (w_req->page), LOCK_WRITE);
	if (cache_entry_to_write == CACHE_MISS) {
		cache_entry_to_write = assign_cache_entry((w_req->pid));
	}
	update_cache(cache_entry_to_write, (w_req->pid), (w_req->page), (w_req->offset), (w_req->size), (w_req->buffer));
	rw_lock_unlock(cache_memory_locks, UNLOCK, cache_entry_to_write);
	return EXIT_SUCCESS;
}

/**
 * @NAME get_cache_entry
 * @DESC
 * @PARAMS
 * @RETURN
 */
int get_cache_entry(int pid, int page, int read_write) {
	t_cache_memory * cache_memory_ptr = (t_cache_memory *) cache_memory;
	int cache_entry = 0;
	while (cache_entry < (memory_conf->cache_entries)) {
		rw_lock_unlock(cache_memory_locks, read_write, cache_entry);
		if ((cache_memory_ptr->pid) == pid && (cache_memory_ptr->page) == page) break;
		rw_lock_unlock(cache_memory_locks, UNLOCK, cache_entry);
		cache_entry++;
		cache_memory_ptr++;
	}
	return (cache_entry < (memory_conf->cache_entries)) ? cache_entry : CACHE_MISS;
}

/**
 * @NAME assign_cache_entry
 * @DESC
 * @PARAMS
 * @RETURN
 */
int assign_cache_entry(int pid) {

	pthread_mutex_lock(&mutex_lock);

	bool exist_on_list = false;
	bool max_entries_exceeded = false;
	t_cache_x_process * cache_x_process;
	if ((cache_x_process_list->elements_count) > 0) { // searching cache entries per process
		int pos = 0;
		while (pos < (cache_x_process_list->elements_count)) {
			cache_x_process = (t_cache_x_process *) list_get(cache_x_process_list, pos);
			if ((cache_x_process->pid) == pid) {
				exist_on_list = true;
				if ((cache_x_process->entries) >= ((memory_conf->cache_x_process))) {
					max_entries_exceeded = true;
				} else {
					(cache_x_process->entries)++;
				}
				break;
			}
			pos++;
		}
	}

	if (!exist_on_list) {
		// process  doesn't exist on list
		// creating node for list
		cache_x_process = (t_reg_pages_process_table *) malloc (sizeof(t_reg_pages_process_table));
		cache_x_process->pid = pid;
		cache_x_process->entries= 1;
		list_add(cache_x_process_list, cache_x_process);
	}

	pthread_mutex_unlock(&mutex_lock);

	t_cache_memory * cache_memory_ptr = (t_cache_memory *) cache_memory;
	int lru_cache_entry = -1;
	int cache_entry = 0;
	while (cache_entry < (memory_conf->cache_entries)) {
		rw_lock_unlock(cache_memory_locks, LOCK_WRITE, cache_entry);
		if (max_entries_exceeded) {
			if ((cache_memory_ptr->pid) == pid) {
				least_recently_used(cache_entry, &lru_cache_entry);
			} else {
				rw_lock_unlock(cache_memory_locks, UNLOCK, cache_entry);
			}
		} else {
			if ((cache_memory_ptr->pid) < 0) {
				if (lru_cache_entry >= 0) rw_lock_unlock(cache_memory_locks, UNLOCK, lru_cache_entry);
				return cache_entry;
			} else {
				least_recently_used(cache_entry, &lru_cache_entry);
			}
		}
		cache_entry++;
		cache_memory_ptr++;
	}

	return lru_cache_entry;

}

/**
 * @NAME least_recently_used
 * @DESC
 * @PARAMS
 * @RETURN
 */
int least_recently_used(int cache_entry, int * lru_cache_entry) {
	if ((*lru_cache_entry) < 0) {
		*lru_cache_entry = cache_entry;
	} else {
		char ** last_used_time_ptr = (char **) last_used_time;
		last_used_time_ptr += cache_entry;
		char ** lru_time_ptr = (char **) last_used_time;
		lru_time_ptr += (*lru_cache_entry);
		if (strcmp(*last_used_time_ptr, *lru_time_ptr) < 0) {
			rw_lock_unlock(cache_memory_locks, UNLOCK, (*lru_cache_entry));
			*lru_cache_entry = cache_entry;
		} else {
			rw_lock_unlock(cache_memory_locks, UNLOCK, cache_entry);
		}
	}
	return EXIT_SUCCESS;
}

/**
 * @NAME update_cache
 * @DESC
 * @PARAMS
 * @RETURN
 */
int update_cache(int cache_entry, int pid, int page, int offset, int size, void * buffer) {
	t_cache_memory * cache_memory_ptr = (t_cache_memory *) cache_memory;
	cache_memory_ptr += cache_entry;
	char ** last_used_time_ptr = (char **) last_used_time;
	last_used_time_ptr += cache_entry;

	void * cache_write_pos = (cache_memory_ptr->content) + offset;
	memcpy(cache_write_pos, buffer, size);
	cache_memory_ptr->pid = pid;
	cache_memory_ptr->page = page;

	*last_used_time_ptr = temporal_get_string_time();

	return EXIT_SUCCESS;
}

/**
 * @NAME writing_memory
 * @DESC
 * @PARAMS
 * @RETURN
 */
int writing_memory(t_write_request * w_req) {

	// TODO : retardo por acceso a memoria

	// getting associated frame
	int frame = get_frame((w_req->pid), (w_req->page), LOCK_WRITE);
	// getting write position
	void * write_pos = (memory_ptr + (frame * (memory_conf->frame_size)) + ((w_req->offset)));
	// writing
	memcpy(write_pos, (w_req->buffer), (w_req->size));
	rw_lock_unlock(memory_locks, UNLOCK, frame);
	return EXIT_SUCCESS;
}

/**
 * @NAME get_frame
 * @DESC
 * @PARAMS
 * @RETURN
 */
int get_frame(int pid, int page, int read_write) {

	// TODO : implementar función hash para realizar la búsqueda dentro de la tabla de páginas invertida

	t_reg_invert_table * invert_table_ptr = (t_reg_invert_table *) invert_table;
	int frame = 0;
	while (frame < (memory_conf->frames)) {
		rw_lock_unlock(memory_locks, read_write, frame);
		if (((invert_table_ptr->pid) == pid) && ((invert_table_ptr->page) == page)) {
			break;
		} else {
			rw_lock_unlock(memory_locks, UNLOCK, frame);
			frame++;
			invert_table_ptr++;
		}
	}

	return frame;
}

/**
 * @NAME read_page
 * @DESC
 * @PARAMS
 */
void read_page(int * client_socket) {
	t_read_request * r_req = read_recv_req(client_socket, logger);
	if ((r_req->exec_code) == DISCONNECTED_CLIENT) return;

	if (reading_cache(client_socket, r_req) == CACHE_MISS)
		readind_memory(client_socket,r_req);

	free(r_req);
}

/**
 * @NAME reading_cache
 * @DESC
 * @PARAMS
 * @RETURN
 */
int reading_cache(int * client_socket, t_read_request * r_req) {
	int cache_entry = get_cache_entry((r_req->pid), (r_req->page), LOCK_READ);
	if (cache_entry == CACHE_MISS) {
		return CACHE_MISS;
	}
	read_cache_entry_and_send(client_socket, r_req, cache_entry);
	rw_lock_unlock(cache_memory_locks, UNLOCK, cache_entry);
	return CACHE_HIT;
}

/**
 * @NAME read_cache_entry_and_send
 * @DESC
 * @PARAMS
 * @RETURN
 */
int read_cache_entry_and_send(int * client_socket, t_read_request * r_req, int cache_entry) {
	t_cache_memory * cache_memory_ptr = (t_cache_memory *) cache_memory;
	cache_memory_ptr += cache_entry;
	char ** last_used_time_ptr = (char **) last_used_time;
	last_used_time_ptr += cache_entry;

	void * buffer = malloc(sizeof(char) * (r_req->size));
	memcpy(buffer, (cache_memory_ptr->content) + (r_req->offset), (r_req->size));
	read_send_resp(client_socket, SUCCESS, (r_req->size), buffer);
	free(buffer);

	*last_used_time_ptr = temporal_get_string_time();

	return EXIT_SUCCESS;
}

/**
 * @NAME readind_memory
 * @DESC
 * @PARAMS
 * @RETURN
 */
int readind_memory(int * client_socket, t_read_request * r_req) {
	int frame = get_frame((r_req->pid), (r_req->page), LOCK_READ);
	void * memory_read_pos = memory_ptr +  (frame * (memory_conf->frame_size));

	// updating cache
	int lru_cache_entry = assign_cache_entry((r_req->pid));
	update_cache(lru_cache_entry, (r_req->pid), (r_req->page), 0, (memory_conf->frame_size), memory_read_pos);
	rw_lock_unlock(cache_memory_locks, UNLOCK, lru_cache_entry);

	memory_read_pos += (r_req->offset);
	void * buffer = malloc(sizeof(char) * (r_req->size));
	memcpy(buffer, memory_read_pos, (r_req->size));
	read_send_resp(client_socket, SUCCESS, (r_req->size), buffer);
	free(buffer);
	rw_lock_unlock(memory_locks, UNLOCK, frame);

	return EXIT_SUCCESS;
}

/**
 * @NAME assign_page
 * @DESC
 * @PARAMS
 */
void assign_page(int * client_socket) {
	t_assign_pages_request * assign_req = init_process_recv_req(client_socket, logger);
	if ((assign_req->exec_code) == DISCONNECTED_CLIENT) return;
	int resp_code = assign_pages_to_process((assign_req->pid), (assign_req->pages));
	free(assign_req);
	init_process_send_resp(client_socket, resp_code);
}

/**
 * @NAME finalize_process
 * @DESC
 * @PARAMS
 */
void finalize_process(int * client_socket) {
	t_finalize_process_request * finalize_req = finalize_process_recv_req(client_socket, logger);
	if ((finalize_req->exec_code) == DISCONNECTED_CLIENT) return;
	cleaning_process_entries((finalize_req->pid));
	finalize_process_send_resp(client_socket, SUCCESS);
}

/**
 * @NAME cleaning_process_entries
 * @DESC
 * @PARAMS
 * @RETURN
 */
int cleaning_process_entries(int pid) {

	pthread_mutex_lock(&mutex_lock);

	//
	// MEMORY
	//
	t_reg_invert_table * invert_table_ptr = (t_reg_invert_table *) invert_table;
	int frame = 0;
	while (frame < (memory_conf->frames)) {
		if (((invert_table_ptr->pid) == pid)) {
			invert_table_ptr->pid = 0;
			invert_table_ptr->page = 0;
			list_add(available_frame_list, frame);
		}
		frame++;
		invert_table_ptr++;
	}

	t_reg_pages_process_table * pages_per_process;
	int index = 0;
	while (index < (pages_per_process_list->elements_count)) { // searching process on pages per process list
		pages_per_process = (t_reg_pages_process_table *) list_get(pages_per_process_list, index);
		if ((pages_per_process->pid) == pid) {
			pages_per_process = list_remove(pages_per_process_list, index);
			free(pages_per_process);
			break;
		}
		index++;
	}

	//
	// CACHE MEMORY
	//
	t_cache_memory * cache_memory_ptr = (t_cache_memory *) cache_memory;
	char ** last_used_time_ptr = (char **) last_used_time;
	int cache_entry = 0;
	while (cache_entry < (memory_conf->cache_entries)) {
		rw_lock_unlock(cache_memory_locks, LOCK_WRITE, cache_entry);
		if ((cache_memory_ptr->pid) == pid) {
			cache_memory_ptr->pid = -1;
			cache_memory_ptr->page = -1;
			memcpy((cache_memory_ptr->content), "\0", 1);
			*last_used_time_ptr = "\0";
		}
		rw_lock_unlock(cache_memory_locks, UNLOCK, cache_entry);
		cache_entry++;
		cache_memory_ptr++;
		last_used_time_ptr++;
	}

	t_cache_x_process * cache_x_process;
	index = 0;
	while (index < (cache_x_process_list->elements_count)) { // searching process on cache per process list
		cache_x_process = (t_cache_x_process *) list_get(cache_x_process_list, index);
		if ((cache_x_process->pid) == pid) {
			cache_x_process = list_remove(cache_x_process_list, index);
			free(cache_x_process);
			break;
		}
		index++;
	}

	pthread_mutex_unlock(&mutex_lock);
	return EXIT_SUCCESS;
}


/**
 * @NAME memory_console
 * @DESC
 */
void memory_console(void * unused) {
	char * input = NULL;
	char * command = NULL;
	char * param = NULL;
	size_t len = 0;
	ssize_t read;
	while ((read = getline(&input, &len, stdin)) != -1) {
		if (read > 0) {
			input[read-1] = '\0';
			char * token = strtok(input, " ");
			if (token != NULL) command = token;
			token = strtok(NULL, " ");
			if (token != NULL) param = token;
			if (strcmp(command, DELAY_CMD) == 0) {
				log_info(console, "delay command");
				uint32_t new_delay_value = atoi(param);
				if (new_delay_value > 0) {
					pthread_mutex_lock(&mutex_lock);
					memory_conf->memory_delay = new_delay_value;
					pthread_mutex_unlock(&mutex_lock);
					log_info(console, "delay value %d", memory_conf->memory_delay);
				} else {
					log_info(console, "invalid value");
				}
			} else if (strcmp(command, DUMP_CMD) == 0) {
				log_info(console, "dump command");
				if (strcmp(param, DUMP_CACHE_CMD) == 0) {
					log_info(console, "cache content");
					t_cache_memory * cache_memory_ptr = (t_cache_memory *) cache_memory;
					char ** last_used_time_ptr = (char **) last_used_time;
					log_info(console, "      #pid        #page   #last used time   #content");
					log_info(console, "__________   __________   _______________   ");
					int cache_entry = 0;
					char * chache_content;
					while (cache_entry < (memory_conf->cache_entries)) {
						rw_lock_unlock(cache_memory_locks, LOCK_READ, cache_entry);
						chache_content = malloc((memory_conf->frame_size) + 1);
						memcpy(chache_content, (cache_memory_ptr->content), (memory_conf->frame_size));
						chache_content[(memory_conf->frame_size)] = "\0";
						log_info(console, "%10d | %10d | %15s | %s", (cache_memory_ptr->pid), (cache_memory_ptr->page), (*last_used_time_ptr), chache_content);
						free(chache_content);
						rw_lock_unlock(cache_memory_locks, UNLOCK, cache_entry);
						cache_entry++;
						cache_memory_ptr++;
						last_used_time_ptr++;
					}
				} else if (strcmp(param, DUMP_MEMORY_STRUCT_CMD) == 0) {
					log_info(console, "invert table");
					log_info(console, "    #frame         #pid        #page");
					log_info(console, "__________   __________   __________");
					t_reg_invert_table * invert_table_ptr = (t_reg_invert_table *) invert_table;
					int frame = 0;
					pthread_mutex_lock(&mutex_lock);
					while (frame < (memory_conf->frames)) {
						log_info(console, "%10d | %10d | %10d", (invert_table_ptr->frame), (invert_table_ptr->pid), (invert_table_ptr->page));
						frame++;
						invert_table_ptr++;
					}
					pthread_mutex_unlock(&mutex_lock);

				} else if (strcmp(param, DUMP_MEMORY_CONTENT_CMD) == 0) {
					printf("\nmemory content");
					t_reg_invert_table * invert_table_ptr = (t_reg_invert_table *) invert_table;
					int frame = 0;
					while (frame < (memory_conf->frames) && (invert_table_ptr->pid < 0)) {
						frame++;
						invert_table_ptr++;
					}
					void * read_pos = memory_ptr + (frame * (memory_conf->frame_size));
					char * frame_content;
					printf("\n    #frame   #content");
					printf("\n__________   ");
					int i;
					while (frame < (memory_conf->frames)) {
						rw_lock_unlock(memory_locks, LOCK_READ, frame);
						frame_content = malloc((memory_conf->frame_size));
						memcpy(frame_content, read_pos, (memory_conf->frame_size));
						printf("\n%10d | ", frame);
						for (i = 0; i < (memory_conf->frame_size); i++) {
							if (frame_content[i] == '\0') {
								printf("%s", "\\0");
							} else if (frame_content[i] == '\n') {
								printf("%s", "\\n");
							} else {
								printf("%c", frame_content[i]);
							}
						};
						free(frame_content);
						rw_lock_unlock(memory_locks, UNLOCK, frame);
						frame++;
						read_pos += (memory_conf->frame_size);
					}
					printf("\n");
				}
			} else if (strcmp(command, FLUSH_CMD) == 0) {
				log_info(console, "flush command");
				t_cache_memory * cache_memory_ptr = (t_cache_memory *) cache_memory;
				char ** last_used_time_ptr = (char **) last_used_time;
				int cache_entry = 0;
				t_cache_x_process * cache_x_process;
				int index;
				while (cache_entry < (memory_conf->cache_entries)) {
					rw_lock_unlock(cache_memory_locks, LOCK_WRITE, cache_entry);
					index = 0;
					while (index < (cache_x_process_list->elements_count)) {
						cache_x_process = list_get(cache_x_process_list, 0);
						if ((cache_x_process->pid) == (cache_memory_ptr->pid)) {
							(cache_x_process->entries)--;
							if (cache_x_process->entries <= 0) {
								cache_x_process = list_remove(cache_x_process_list, 0);
								free(cache_x_process);
							}
							break;
						}
					}
					cache_memory_ptr->pid = -1;
					cache_memory_ptr->page = -1;
					memcpy((cache_memory_ptr->content), "\0", 1);
					*last_used_time_ptr = "\0";
					rw_lock_unlock(cache_memory_locks, UNLOCK, cache_entry);
					cache_entry++;
					cache_memory_ptr++;
					last_used_time_ptr++;
				}
				log_info(console, "flush finished");
			} else if (strcmp(command, SIZE_CMD) == 0) {
				log_info(console, "size command");
				if (strcmp(param, SIZE_MEMORY_CMD) == 0) {
					log_info(console, "memory size");
					log_info(console, "frames ------------> %d", (memory_conf->frames));
					log_info(console, "available frames --> %d", (available_frame_list->elements_count));
					log_info(console, "busy frames -------> %d", ((memory_conf->frames) - (available_frame_list->elements_count)));
				} else {
					uint32_t pid = atoi(param);
					if (pid >= 0) {
						pthread_mutex_lock(&mutex_lock);
						t_reg_pages_process_table * pages_per_process;
						int index = 0;
						while (index < (pages_per_process_list->elements_count)) { // searching process on pages per process list
							pages_per_process = (t_reg_pages_process_table *) list_get(pages_per_process_list, index);
							if ((pages_per_process->pid) == pid) {
								log_info(console, "pid : %d", pid);
								log_info(console, "frames ------------> %d", (pages_per_process->pages_count));
								log_info(console, "size --------------> %d bytes", (pages_per_process->pages_count) * (memory_conf->frame_size));
								break;
							}
							index++;
						}
						pthread_mutex_unlock(&mutex_lock);
					} else {
						log_info(console, "invalid pid");
					}
				}
			}
		}
	}
	free(command);
}
