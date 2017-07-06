/*
 * kernel_generales.h
 *
 *  Created on: 17/5/2017
 *      Author: utnso
 */

#ifndef KERNEL_GENERALES_H_
#define KERNEL_GENERALES_H_

#include <commons/collections/list.h>
#include <commons/collections/queue.h>
#include <commons/config.h>
#include <shared-library/generales.h>

typedef struct{
	int program_port;
	int cpu_port;
	char* memory_ip;
	char* memory_port;
	char* filesystem_ip;
	char* filesystem_port;
	int quantum;
	int quantum_sleep;
	char* algoritmo;
	int grado_multiprog;
	char** sem_ids;
	char** sem_init;
	char* shared_vars;
	int stack_size;
}t_kernel_conf;

typedef struct{
	int file_descriptor;
	t_PCB* proceso_asignado;
	int quantum;
}t_cpu;

typedef struct{
	int port;
	fd_set* master;
//	fd_set lectura;
}t_aux;

typedef struct{
	int pid;
	int paginas_codigo;
}t_codigo_proceso;

typedef struct {
	int pid;
	int socket;
} t_par_socket_pid;

typedef struct{
	int file_descriptor;
	fd_set* set;
}t_info_socket_solicitud;

fd_set master_cpu, master_prog;
int registro_pid;
t_log* logger;
t_queue* cola_nuevos;
t_queue* cola_listos;
t_queue* cola_bloqueados;
t_queue* cola_exit;
t_queue* cola_cpu;
t_list* tabla_archivos;
t_queue* cola_finalizados;
t_list* lista_cpu;
t_queue* cola_ejecutando;
int memory_socket, fs_socket;
t_kernel_conf* kernel_conf;
t_list* tabla_paginas_por_proceso;
int TAMANIO_PAGINAS;
t_list * tabla_sockets_procesos;
t_dictionary* semaforos;
t_list* tabla_variables_compartidas;

/**
 * @NAME: crear_PCB
 * @DESC: Crea instancia de pcb y le asigna pid unico.
 */
t_PCB* crear_PCB();

void load_kernel_properties(void);
void crearVariablesCompartidas(void);

//CPU
t_cpu* cpu_obtener_libre(t_list* lista_cpu);
void cpu_enviar_pcb(t_cpu* cpu, t_PCB* pcb);

#endif /* KERNEL_GENERALES_H_ */
