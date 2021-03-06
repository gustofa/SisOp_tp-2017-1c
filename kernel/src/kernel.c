/*
 ============================================================================
 Name        : cpu.c
 Authors     : Carlos Flores, Gustavo Tofaletti, Dante Romero
 Version     :
 Description : Kernel Proccess
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <shared-library/socket.h>
#include <shared-library/generales.h>
#include <shared-library/memory_prot.h>
#include <pthread.h>
#include <sys/inotify.h>
#include "kernel.h"
#include "solicitudes.h"

#define EVENT_SIZE ( sizeof (struct inotify_event) + 256 )
#define BUF_LEN ( 1024 * EVENT_SIZE )

t_par_socket_pid* buscar_proceso_por_socket(int socket);

int main(int argc, char* argv[]) {
	pthread_mutex_init(&mutex_pedido_memoria, NULL);

	registro_pid = 1;

	crear_logger(argv[0], &logger, false, LOG_LEVEL_TRACE);
	log_trace(logger, "Log Creado!!");

	contador_fd_global = 10;

	tabla_variables_compartidas = list_create();
	load_kernel_properties(argv[1]);
	listaDeTablasDeArchivosDeProcesos = list_create();
	tabla_paginas_heap = list_create();
	tabla_paginas_por_proceso = list_create();
	tabla_sockets_procesos = list_create();

	cola_nuevos = queue_create();
	cola_bloqueados = queue_create();
	cola_ejecutando = queue_create();
	cola_finalizados = queue_create();
	cola_listos = queue_create();

	semColaBloqueados = malloc(sizeof(sem_t));
	semColaFinalizados = malloc(sizeof(sem_t));
	//semColaListos = malloc(sizeof(sem_t));
	semColaNuevos = malloc(sizeof(sem_t));
	semPlanificarCortoPlazo = malloc(sizeof(sem_t));
	semPlanificarLargoPlazo = malloc(sizeof(sem_t));
	semCantidadProgramasPlanificados = malloc(sizeof(sem_t));
	semCantidadElementosColaListos = malloc(sizeof(sem_t));
	semCantidadCpuLibres = malloc(sizeof(sem_t));
	semListaCpu = malloc(sizeof(sem_t));
	lock_tabla_global_archivos = malloc(sizeof(pthread_rwlock_t));
	semSemaforos = malloc(sizeof(sem_t));

	sem_init(semColaBloqueados, 0, 1);
	pthread_mutex_init(&semColaListos, NULL);
	sem_init(semColaNuevos, 0, 1);
	sem_init(semColaFinalizados, 0, 1);
	sem_init(semPlanificarCortoPlazo, 0, 0);
	sem_init(semPlanificarLargoPlazo, 0, 0);
	sem_init(semCantidadProgramasPlanificados, 0, 0);
	sem_init(semCantidadElementosColaListos, 0, 0);
	sem_init(semCantidadCpuLibres, 0, 0);
	sem_init(semListaCpu, 0, 1);
	pthread_mutex_init(lock_tabla_global_archivos, NULL);
	sem_init(semSemaforos, 0, 1);

	pthread_mutex_init(&mutex_planificar, NULL);
	pthread_mutex_init(&mutex_kill, NULL);

	planificar = true;
	int ret;
	ret = sem_init(&sem_planificar_largo_plazo, 0, 0);
	ret = sem_init(&sem_planificar_corto_plazo, 0, 0);

	lista_cpu = list_create();
	rw_lock_unlock(UNLOCK);


	memory_socket = connect_to_socket(kernel_conf->memory_ip, kernel_conf->memory_port);


	TAMANIO_PAGINAS = handshake(memory_socket,'k', kernel_conf->stack_size, logger);
	fs_socket = connect_to_socket(kernel_conf->filesystem_ip, kernel_conf->filesystem_port);
	fs_handshake(&fs_socket, logger);


	tabla_global_archivos = list_create();


	pthread_t hilo_cpu;
	pthread_t hilo_consola;
	pthread_t hilo_pantalla;

	t_aux *estruc_cpu, *estruc_prog;
	estruc_cpu = malloc(sizeof(t_aux));
	estruc_prog = malloc(sizeof(t_aux));
	estruc_cpu->port = kernel_conf->cpu_port;
	estruc_cpu->master = &master_cpu;
	estruc_prog->port = kernel_conf->program_port;
	estruc_prog->master = &master_prog;

	pthread_attr_t attr;

	pthread_attr_init(&attr);

	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	// Se crea hilo planificador corto plazo
	pthread_create(&hilo_pantalla, &attr, &iniciar_consola, NULL);

	pthread_attr_destroy(&attr);

	// Se crea hilo de cpu's
	pthread_create(&hilo_cpu, NULL, &manage_select, estruc_cpu);

	// Se crea hilo de consolas
	pthread_create(&hilo_consola, NULL, &manage_select, estruc_prog);

	// Hilo planificador
	kernel_planificacion();

	monitoriar_config();

	pthread_join(hilo_consola, NULL);
	pthread_join(hilo_cpu, NULL);

	return EXIT_SUCCESS;
}

t_stack* stack_create(){
	t_stack* stack = list_create();
	return stack;
}


t_cpu* cpu_create(int file_descriptor){
	t_cpu* cpu = malloc(sizeof(t_cpu));
	cpu->libre = true;
	cpu->file_descriptor = file_descriptor;
	cpu->proceso_asignado = NULL;
	cpu->matar_proceso=0;
	cpu->proceso_desbloqueado_por_signal=0;
	cpu->proceso_bloqueado_por_wait = 0;
	cpu->quantum=0;
	return cpu;
}

void manage_select(t_aux* estructura){

	int listening_socket;
	listening_socket = open_socket(20, estructura->port);
	int nuevaConexion, fd_seleccionado, recibido, set_fd_max, i;
	uint8_t operation_code;
	char* buffer;
	int status;
	fd_set lectura;
	pthread_attr_t attr;
	set_fd_max = listening_socket;
	FD_ZERO(&lectura);
	FD_ZERO((estructura->master));
	FD_SET(listening_socket, (estructura->master));
	while(1){
		lectura = *(estructura->master);
		select(set_fd_max +1, &lectura, NULL, NULL, NULL);
		for(fd_seleccionado = 0 ; fd_seleccionado <= set_fd_max ; fd_seleccionado++){
			if(FD_ISSET(fd_seleccionado, &lectura)){
				if(fd_seleccionado == listening_socket){
					if((nuevaConexion = accept_connection(listening_socket)) == -1){
						log_error(logger, "Error al aceptar conexion");
					} else {
						log_trace(logger, "Nueva conexion: socket %d", nuevaConexion);
						FD_SET(nuevaConexion, (estructura->master));
						if(nuevaConexion > set_fd_max)set_fd_max = nuevaConexion;
						if(estructura->port == kernel_conf->cpu_port){
							t_cpu* cpu = cpu_create(nuevaConexion);
							connection_send(nuevaConexion, HANDSHAKE_CPU, &(kernel_conf->stack_size));
							sem_wait(semListaCpu);
							list_add(lista_cpu, cpu);
							sem_post(semListaCpu);
							sem_post(semCantidadCpuLibres);


						}
					}
				} else {
					pthread_t hilo_solicitud;

					/*****************************************************************************************
					 * Esto soluciona el P*** ERROR de sincronizacion que hacia que rompa .todo. cuando se   *
					 * conectaba una segunda CPU!! ya que al haber echo el MALLOC en la parte de arriba era  *
					 * el mismo puntero para todos los hilos                                                 *
					 *****************************************************************************************/
					t_info_socket_solicitud* info_solicitud = malloc(sizeof(t_info_socket_solicitud));

					info_solicitud->file_descriptor = fd_seleccionado;
					info_solicitud->set = estructura->master;
					info_solicitud->lectura = &lectura;

					status = connection_recv(fd_seleccionado, &operation_code, &buffer);

					info_solicitud->oc_code = operation_code;
					info_solicitud->buffer = buffer;

					if(status <= 0 ){
						//si es una desconexion ni si quiera creo los hilos
						FD_CLR(fd_seleccionado, estructura->master);

						if(estructura->port == kernel_conf->cpu_port){
							t_cpu* cpu = obtener_cpu(fd_seleccionado);
							if(cpu->proceso_asignado){
								cola_listos_push(cpu->proceso_asignado);
							}
							eliminar_cpu(fd_seleccionado);
						} else if(estructura->port == kernel_conf->program_port){

							t_par_socket_pid* par = buscar_proceso_por_socket(fd_seleccionado);
							while(par!= NULL){
		//						memory_finalize_process(memory_socket, par->pid, logger);
								pasar_proceso_a_exit(par->pid);
								par = buscar_proceso_por_socket(fd_seleccionado);
							}
						}

					}else{
						pthread_attr_init(&attr);

						pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

						pthread_create(&hilo_solicitud, &attr, &solve_request, info_solicitud);

						pthread_attr_destroy(&attr);
					}
				}
			}
		}
	}
}


void kernel_planificacion() {

	pthread_t hilo_corto_plazo;
	pthread_t hilo_largo_plazo;

	pthread_attr_t attr;

	pthread_attr_init(&attr);

	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	// Se crea hilo planificador corto plazo
	pthread_create(&hilo_corto_plazo, &attr, &planificador_largo_plazo, 0);

	// Se crea hilo planificador largo plazo
	pthread_create(&hilo_largo_plazo, &attr, &planificador_corto_plazo, 0);

	pthread_attr_destroy(&attr);
}

void planificador_largo_plazo(){
	while(true){
		if(!planificar) sem_wait(&sem_planificar_largo_plazo);

		sem_wait(semPlanificarLargoPlazo);
		pasarDeNewAReady();
	}
}

/**
 * planifica sólo si tiene elementos en la cola de listos
 * y si tiene al menos una cpu libre
 */
void planificador_corto_plazo(){
	while(true){
		// se usa para habilitar/deshabilitar la planificacion
		if(!planificar) sem_wait(&sem_planificar_corto_plazo);

		pasarDeReadyAExecute();
	}
}

void pasarDeNewAReady(){
	t_PCB* pcb;
	int respuesta;
	t_nuevo_proceso* nuevo_proceso;
	int cantidadProgramasPlanificados;
	sem_getvalue(semCantidadProgramasPlanificados, &cantidadProgramasPlanificados);
	//  no hace nada si el grado de multiprogramacion no se lo permite
	if(cantidadProgramasPlanificados < kernel_conf->grado_multiprog && queue_size(cola_nuevos) > 0){
		sem_wait(semColaNuevos);
		nuevo_proceso = queue_pop(cola_nuevos);
		sem_post(semColaNuevos);
		respuesta = notificar_memoria_inicio_programa(nuevo_proceso->pcb->pid, nuevo_proceso->cantidad_paginas, nuevo_proceso->codigo);
		if(respuesta == ENOSPC){
			nuevo_proceso->pcb->exit_code = -1;
			// la funcion pasarDeNewAReady() termina pasando a finalizados!? (que asco...)
			queue_push(cola_finalizados, nuevo_proceso->pcb);
			sem_post(semPlanificarLargoPlazo);
		} else {
			cola_listos_push(nuevo_proceso->pcb);
			sem_post(semCantidadProgramasPlanificados);
			liberar_nuevo_proceso(nuevo_proceso);
			if(cantidadProgramasPlanificados<kernel_conf->grado_multiprog)sem_post(semPlanificarLargoPlazo);
		}
	}
}

void pasarDeReadyAExecute(){
	bool serializarPCB = false;
	t_cpu* cpu = NULL;
	t_PCB* pcb;
	int i;

	sem_wait(semCantidadElementosColaListos);
	sem_wait(semCantidadCpuLibres);
	pthread_mutex_lock(&semColaListos);
	log_trace(logger, "list_size de cola_listos %d", list_size(cola_listos));
	pcb = queue_pop(cola_listos);
	if(pcb)	log_trace(logger, "POP (pid: %d - PC: %d - SP: %d POS %p) en Ready", pcb->pid, pcb->PC, pcb->SP, pcb);
	else log_trace(logger, "POP pcb NULL en Ready");

	pthread_mutex_unlock(&semColaListos);

	sem_wait(semListaCpu);
	for (i = 0; i < list_size(lista_cpu); i++) {
		cpu = list_get(lista_cpu, i);
		if( cpu->libre ) {
			/*sem_wait(semColaListos);
			pcb = queue_pop(cola_listos);
			sem_post(semColaListos);*/
			// si no tiene procesos en la cola de listos no hace nada
			if(pcb != NULL){
				cpu->libre = false;
				cpu->proceso_asignado = pcb;
				serializarPCB = true;
			} else {
				sem_post(semCantidadCpuLibres);
				log_error(logger, "Fallo al obtener PCB en planificador corto plazo");
			}
			break;
		}
	}
	sem_post(semListaCpu);

	if (serializarPCB) {
		serializar_y_enviar_PCB(pcb, cpu->file_descriptor, OC_PCB);
	}
}

void pasarDeExecuteAReady(t_cpu* cpu){
	t_PCB* pcb = cpu->proceso_asignado;
	liberar_cpu(cpu);
	cola_listos_push(pcb); //se invierte el orden porque esta funcion corre el planificador

}

void pasarDeExecuteAExit(t_cpu* cpu){
	sem_wait(semColaFinalizados);
	queue_push(cola_finalizados, cpu->proceso_asignado);
	liberar_cpu(cpu);
	sem_post(semColaFinalizados);
	sem_post(semPlanificarLargoPlazo);
	sem_wait(semCantidadProgramasPlanificados);
}

void enviar_a_ejecutar(t_cpu* cpu){

}

void actualizar_quantum_sleep(){
	t_config * conf = config_create("./kernel.cfg");
	kernel_conf->quantum_sleep = config_get_int_value(conf, "QUANTUM_SLEEP");
	config_destroy(conf);
}

void cola_listos_push(t_PCB *element){
	pthread_mutex_lock(&semColaListos);
	queue_push(cola_listos, element);
	log_trace(logger, "PUSH (pid: %d - PC: %d - SP: %d) en Ready 422 kernel.c", element->pid, element->PC, element->SP);

	log_trace(logger, "PID %d - Se inserta en Ready", element->pid);
	sem_post(semCantidadElementosColaListos);
	pthread_mutex_unlock(&semColaListos);
}

void eliminar_cpu(int file_descriptor){
	bool _is_cpu(t_cpu* cpu){
		return (cpu->file_descriptor == file_descriptor);
	}

	list_remove_and_destroy_by_condition(lista_cpu, _is_cpu, free);
}


t_par_socket_pid* buscar_proceso_por_socket(int socket){
	bool tiene_mismo_socket(t_par_socket_pid* p){
		return p->socket == socket;
	}
	return list_remove_by_condition(tabla_sockets_procesos, (void*) tiene_mismo_socket);
}

void monitoriar_config(){

	char buffer[BUF_LEN];
	int notificador =  inotify_init();
	int watch_descriptor = inotify_add_watch(notificador, "./", IN_MODIFY | IN_CLOSE_WRITE );
	while(1){
		int length = read(notificador, buffer, BUF_LEN);
		if (length < 0) {
			perror("read");
		}
		int offset = 0;
		while (offset < length) {
			struct inotify_event *event = (struct inotify_event *) &buffer[offset];
			if (event->len) {
				if (event->mask & IN_MODIFY) {
					t_config * conf = config_create("./kernel.cfg");
					if(config_has_property(conf, "QUANTUM_SLEEP")){
						kernel_conf->quantum_sleep = config_get_int_value(conf, "QUANTUM_SLEEP");
					}
				} else if(event->mask & IN_CLOSE_WRITE){
					t_config * conf = config_create("./kernel.cfg");
					if(config_has_property(conf, "QUANTUM_SLEEP")){
						kernel_conf->quantum_sleep = config_get_int_value(conf, "QUANTUM_SLEEP");
					}
				}
			}
			offset += sizeof (struct inotify_event) + event->len;
		}
	}
}
