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
#include <semaphore.h>


#define	LOCK_READ 				0
#define	LOCK_WRITE 				1
#define	UNLOCK 					2

#define PLANIFICACION_FIFO "FIFO"
#define PLANIFICACION_ROUND_ROBIN "RR"

bool planificar;

#define MUERE_PROGRAMA 99

sem_t *semColaBloqueados;
sem_t *semPlanificarLargoPlazo;
sem_t *semPlanificarCortoPlazo;
sem_t *semColaNuevos;
pthread_mutex_t semColaListos;
sem_t* semColaFinalizados;
sem_t* semListaCpu;
sem_t *semCantidadCpuLibres;
sem_t *semCantidadProgramasPlanificados;
sem_t *semCantidadElementosColaListos;
sem_t sem_planificar_corto_plazo;
sem_t sem_planificar_largo_plazo;
pthread_mutex_t registro_pid_mutex;
pthread_mutex_t mutex_planificar;
pthread_rwlock_t* lock_tabla_global_archivos;
pthread_mutex_t mutex_pedido_memoria;
pthread_mutex_t mutex_kill;

sem_t* semSemaforos;

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
	bool libre;
	char* nombre_semaforo;
	int file_descriptor;
	t_PCB* proceso_asignado;
	int quantum;
	int matar_proceso; // si es true se debe pasar el pcb a la cola finalizados
	int proceso_desbloqueado_por_signal; // si es true se debe incrementar semaforo que cuenta la cola de listos
	int proceso_bloqueado_por_wait;
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

typedef struct{
	t_PCB* pcb;
	int cantidad_paginas;
	char* codigo;
} t_nuevo_proceso;

typedef struct{
  int file_descriptor;
  fd_set* set;
  fd_set* lectura;
  void * buffer;
  uint8_t oc_code;
}t_info_socket_solicitud;

typedef struct{
	int cuenta;
	t_queue* cola;
}t_semaphore;

typedef struct {
	int pid;
	int socket;
	int cantidad_syscalls;
	int memoria_reservada;
	int memoria_liberada;
} t_par_socket_pid;


fd_set master_cpu, master_prog;
int registro_pid;
t_log* logger;
t_queue* cola_nuevos;
t_queue* cola_listos;
t_queue* cola_bloqueados;
t_list* listaDeTablasDeArchivosDeProcesos;
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
void liberar_cpu(t_cpu* cpu);
t_PCB* sacar_pcb_con_pid(t_queue* cola, uint16_t pid);
t_table_file* getTablaArchivo(int pid);

/**
 * @NAME: crear_PCB
 * @DESC: Crea instancia de pcb y le asigna pid unico.
 */
t_PCB* crear_PCB();

void load_kernel_properties(char* ruta );
void crearVariablesCompartidas(void);

//CPU
//t_cpu* cpu_obtener_libre(t_list* lista_cpu);
void cpu_enviar_pcb(t_cpu* cpu, t_PCB* pcb);
t_cpu* obtener_cpu(int socket);
t_cpu* find_by_fd(int fd);
bool proceso_bloqueado(t_PCB* pcb);
t_PCB* sacar_pcb(t_queue* cola, t_PCB* pcb);
void liberar_nuevo_proceso(t_nuevo_proceso* nuevo_proceso);
void iniciar_consola();
t_cpu* buscar_pcb_en_lista_cpu(t_PCB* pcbABuscar);

void pasarDeBlockedAReady(uint16_t pidPcbASacar);
void pasarDeExecuteABlocked(t_cpu* cpu);
bool continuar_procesando(t_cpu* cpu);

#endif /* KERNEL_GENERALES_H_ */
