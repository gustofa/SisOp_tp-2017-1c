/*
 * solicitudes.c
 *
 *  Created on: 15/5/2017
 *      Author: utnso
 */

#include "solicitudes.h"
#include <parser/metadata_program.h>
#include <parser/parser.h>

extern t_list* listaDeTablasDeArchivosDeProcesos;
extern pthread_rwlock_t* lock_tabla_global_archivos;

void solve_request(t_info_socket_solicitud* info_solicitud){
	uint8_t operation_code = info_solicitud->oc_code;
	char* buffer = info_solicitud->buffer;
	uint32_t cant_paginas, direcc_logica, direcc_fisica;
	t_puntero bloque_heap_ptr, error_heap;
	int *resp = malloc(sizeof(int));
	t_pedido_reservar_memoria* pedido;
	t_pedido_liberar_memoria* liberar;
	t_pagina_heap* pagina;
	t_read_response* respuesta_pedido_pagina;
	t_PCB* pcb;
	t_PCB* auxPCB;
	t_cpu* cpu;
	char* nombre_semaforo, *nombre_variable, *informacion, * direccion, *paquete_respuesta_reservar;
	t_semaphore* semaforo;
	t_metadata_program* metadata;
	t_table_file* tabla_proceso;
	t_heapMetadata* metadata_bloque;
	t_codigo_proceso* info_proceso;
	t_shared_var* variable_recibida;
	t_archivo * escritura;
    int length_direccion, pid, size_nombre, socket_consola, status;
    t_banderas flags;


	switch(operation_code){
	case OC_SOLICITUD_PROGRAMA_NUEVO: {

		cant_paginas = calcular_paginas_necesarias(buffer);
		pcb = crear_PCB();

		t_par_socket_pid * parnuevo = malloc(sizeof(t_par_socket_pid));
		parnuevo->pid = pcb->pid;
		parnuevo->socket = info_solicitud->file_descriptor;
		parnuevo->cantidad_syscalls = 0;
		parnuevo->memoria_liberada = 0;
		parnuevo->memoria_reservada = 0;
		list_add(tabla_sockets_procesos, parnuevo);

		log_trace(logger, "SOCKET DEL PID %d: %d", pcb->pid, info_solicitud->file_descriptor);

		pcb->cantidad_paginas = cant_paginas-kernel_conf->stack_size;
		pcb->PC = 0;

		t_codigo_proceso* paginas_de_codigo = malloc(sizeof(t_codigo_proceso));
		paginas_de_codigo->pid = pcb->pid;
		paginas_de_codigo->paginas_codigo = cant_paginas;
		list_add(tabla_paginas_por_proceso, paginas_de_codigo);

		connection_send(info_solicitud->file_descriptor, OC_NUEVA_CONSOLA_PID, &(pcb->pid));

		metadata = metadata_desde_literal(buffer);

		pcb->SP = 0;
		pcb->cantidad_instrucciones = metadata->instrucciones_size;
		pcb->indice_codigo = obtener_indice_codigo(metadata);
		pcb->indice_etiquetas = obtener_indice_etiquetas(metadata);
		metadata_destruir(metadata);

		//Creo tabla de archivos del proceso
		tabla_proceso = malloc(sizeof(t_table_file));
		tabla_proceso->pid = pcb->pid;
		tabla_proceso->tabla_archivos = crearTablaArchProceso();
		tabla_proceso->contador_fd = 10;
		list_add(listaDeTablasDeArchivosDeProcesos, tabla_proceso);

		t_nuevo_proceso* nuevo_proceso = malloc(sizeof(t_nuevo_proceso));
		nuevo_proceso->cantidad_paginas = cant_paginas;
		nuevo_proceso->codigo = buffer;
		nuevo_proceso->pcb = pcb;

		sem_wait(semColaNuevos);
		queue_push(cola_nuevos, nuevo_proceso);
		sem_post(semColaNuevos);
		sem_post(semPlanificarLargoPlazo);
		break;
	}
	case OC_FUNCION_RESERVAR:


		pedido = (t_pedido_reservar_memoria*)buffer;
		bloque_heap_ptr = 0;
		if(pedido->espacio_pedido > (TAMANIO_PAGINAS - sizeof(t_heapMetadata))){
			log_trace(logger, "Pedido de heap invalido - Se esta pidiendo mas espacio del permitido por solicitud");
			error_heap = 1;

			pthread_mutex_lock(&mutex_pedido_memoria);
			memory_finalize_process(memory_socket, pedido->pid, logger);
			pthread_mutex_unlock(&mutex_pedido_memoria);

			paquete_respuesta_reservar = malloc(sizeof(t_puntero) * 2);		// Mandamos 2 t_punteros el primero el valor del puntero, el segundo el valor del error
			memcpy(paquete_respuesta_reservar, &bloque_heap_ptr, sizeof(t_puntero));
			memcpy(paquete_respuesta_reservar + sizeof(t_puntero), &error_heap, sizeof(t_puntero));
			connection_send(info_solicitud->file_descriptor, OC_RESP_RESERVAR, paquete_respuesta_reservar);
			break;
		}
		log_trace(logger, "PID %d - OP OC_FUNCION_RESERVAR dentro de kernel-solicitudes.c",pedido->pid);
		info_proceso = buscar_codigo_de_proceso(pedido->pid);
		pagina = obtener_pagina_con_suficiente_espacio(pedido->pid, pedido->espacio_pedido);
		if(pagina == NULL){
			log_trace(logger, "PID %d - Asigna paginas heap empieza", pedido->pid);
			pthread_mutex_lock(&mutex_pedido_memoria);
			status = memory_assign_pages(memory_socket, pedido->pid, 1, logger);
			pthread_mutex_unlock(&mutex_pedido_memoria);
			log_trace(logger, "PID %d - Asigna paginas heap termina", pedido->pid);
			if(status < 0){
				if(status == -203){
					error_heap = 2;

					pthread_mutex_lock(&mutex_pedido_memoria);
					memory_finalize_process(memory_socket, pedido->pid, logger);
					pthread_mutex_unlock(&mutex_pedido_memoria);

					paquete_respuesta_reservar = malloc(sizeof(t_puntero) * 2);
					memcpy(paquete_respuesta_reservar, &bloque_heap_ptr, sizeof(t_puntero));
					memcpy(paquete_respuesta_reservar + sizeof(t_puntero), &error_heap, sizeof(t_puntero));
					connection_send(info_solicitud->file_descriptor, OC_RESP_RESERVAR, paquete_respuesta_reservar);
					log_trace(logger, "No se pudo asignar pagina de heap para proceso con pid: %d", pedido->pid);
				} else log_trace(logger, "Se desconecto la memoria");
				break;
			}

			tabla_heap_agregar_pagina(pedido->pid);
			pagina = obtener_pagina_con_suficiente_espacio(pedido->pid, pedido->espacio_pedido);

			t_heapMetadata* meta_pag_nueva =crear_metadata_libre(TAMANIO_PAGINAS);

			// Escribimos la metadata de la nueva pagina en Memoria
			log_trace(logger, "PID %d - Formatea paginas de heap nueva empieza", pedido->pid);
			pthread_mutex_lock(&mutex_pedido_memoria);
			memory_write(memory_socket, pedido->pid, (pagina->nro_pagina + info_proceso->paginas_codigo), 0, sizeof(t_heapMetadata), sizeof(t_heapMetadata), meta_pag_nueva, logger);
			pthread_mutex_unlock(&mutex_pedido_memoria);
			log_trace(logger, "PID %d - Formatea paginas de heap nueva termina", pedido->pid);

			free(meta_pag_nueva);
		}
		log_trace(logger, "PID %d - Lee paginas heap empieza", pedido->pid);
		pthread_mutex_lock(&mutex_pedido_memoria);
		respuesta_pedido_pagina = memory_read(memory_socket, pedido->pid, (pagina->nro_pagina + info_proceso->paginas_codigo), 0, TAMANIO_PAGINAS, logger);
		pthread_mutex_unlock(&mutex_pedido_memoria);
		log_trace(logger, "PID %d - Lee paginas heap termina", pedido->pid);
		bloque_heap_ptr = buscar_bloque_disponible(respuesta_pedido_pagina->buffer, pedido->espacio_pedido);
		log_trace(logger, "PID %d - puntero de bloque: %d", pedido->pid, bloque_heap_ptr);
		marcar_bloque_ocupado(bloque_heap_ptr, respuesta_pedido_pagina->buffer, pedido->espacio_pedido);
		// Mandamos la pagina de heap modificada
		pthread_mutex_lock(&mutex_pedido_memoria);
		memory_write(memory_socket, pedido->pid, (pagina->nro_pagina + info_proceso->paginas_codigo), 0, TAMANIO_PAGINAS, TAMANIO_PAGINAS, respuesta_pedido_pagina->buffer, logger);
		pthread_mutex_unlock(&mutex_pedido_memoria);
		log_trace(logger, "PID %d - Escribe heap en memoria, en pagina: %d",pedido->pid, (pagina->nro_pagina + info_proceso->paginas_codigo));
		modificar_pagina(pagina, pedido->espacio_pedido); // actualizamos los datos de la pagina en la tabla de heap

		// Mandamos puntero al programa que lo pidio
		obtener_direccion_relativa(&bloque_heap_ptr, pagina->nro_pagina, info_proceso->paginas_codigo);	//sumo el offset de las paginas de codigo, stack y heap
		error_heap = 0;
		paquete_respuesta_reservar = malloc(sizeof(t_puntero) * 2);
		memcpy(paquete_respuesta_reservar, &bloque_heap_ptr, sizeof(t_puntero));
		memcpy(paquete_respuesta_reservar + sizeof(t_puntero), &error_heap, sizeof(t_puntero));
		connection_send(info_solicitud->file_descriptor, OC_RESP_RESERVAR, paquete_respuesta_reservar);
		log_trace(logger, "PID %d - Mando a cpu puntero de malloc pedido. Posicion: %d",pedido->pid, bloque_heap_ptr);
	    sumar_syscall(info_solicitud->file_descriptor);
	    sumar_espacio_reservado(pedido->espacio_pedido, info_solicitud->file_descriptor);
		break;
	case OC_FUNCION_LIBERAR:
		liberar = buffer;		// liberar buffer
		log_trace(logger, "PID %d - pedido de liberar punter. Posicion: %d",liberar->pid, liberar->posicion);
		info_proceso = buscar_codigo_de_proceso(liberar->pid);

		obtener_direccion_de_bloque_verdadera(liberar, info_proceso->paginas_codigo); //le saco las paginas de codigo y stack

		log_trace(logger, "PID %d - Yendo a leer pagina de memoria: %d",liberar->pid,(liberar->nro_pagina + info_proceso->paginas_codigo));
		respuesta_pedido_pagina = memory_read(memory_socket, liberar->pid, (liberar->nro_pagina + info_proceso->paginas_codigo), 0, TAMANIO_PAGINAS, logger);
		metadata_bloque = leer_metadata((char*)respuesta_pedido_pagina->buffer + liberar->posicion);
	    sumar_espacio_liberado(liberar->pid, metadata_bloque->size);

	    marcar_bloque_libre(metadata_bloque, (char*)respuesta_pedido_pagina->buffer + liberar->posicion);	// Marcamos la metadata del bloque como LIBRE en la pagina de heap

	    tabla_heap_cambiar_espacio_libre(liberar, metadata_bloque->size);		// registramos el nuevo espacio libre en la tabla de paginas de heap que tiene kernel
		defragmentar(respuesta_pedido_pagina->buffer, liberar);

		status = 1;
		if(pagina_vacia(liberar->pid, liberar->nro_pagina)){
			tabla_heap_sacar_pagina(liberar);
			liberar_pagina(liberar, info_proceso->paginas_codigo);
		    connection_send(info_solicitud->file_descriptor, OC_RESP_LIBERAR, &status);
			break;
		}
		memory_write(memory_socket, liberar->pid, (liberar->nro_pagina + info_proceso->paginas_codigo), 0, TAMANIO_PAGINAS, TAMANIO_PAGINAS, respuesta_pedido_pagina->buffer, logger);
	    sumar_syscall(info_solicitud->file_descriptor);
	    connection_send(info_solicitud->file_descriptor, OC_RESP_LIBERAR, &status);

		break;
	case OC_FUNCION_ABRIR: {
		char* msgerror;
		int direccion_length = *(int*)buffer;
		pid = *(uint16_t*)(buffer + sizeof(int));
		direccion = malloc(direccion_length+1);
//		memcpy(direccion, buffer + sizeof(int) + sizeof(uint16_t), direccion_length);
		strcpy(direccion, buffer + sizeof(int) + sizeof(uint16_t));
		flags = *(t_banderas*)(buffer + sizeof(int) + sizeof(uint16_t) + direccion_length);

		direccion[direccion_length] = '\0';

		//printf("RECIBE EL BUFFER CON LENGTH %d PID %d Y CHAR %s", direccion_length, pid, direccion);
		//log_trace(logger, "RECIBE EL BUFFER CON LENGTH %d PID %d  ", direccion_length, pid);

		int fd_proceso;
		fd_proceso = abrir_archivo(pid, direccion, flags);
	    if(fd_proceso == -1) {
	    	msgerror= strdup("No existe archivo o no tiene permisos");
	    }else if(fd_proceso == ENOSPC) {
	    	fd_proceso=EC_FS_LLENO;
	    	msgerror= strdup("Sin espacio en filesystem");
	    }
    	int _mismoPid(t_par_socket_pid *par){
    		 return par->pid == pid;
    	}
    	if(fd_proceso < 0) {
    	t_par_socket_pid * parNuevo = list_find(tabla_sockets_procesos, (void *) _mismoPid);
    	connection_send(parNuevo->socket, OC_ESCRIBIR_EN_CONSOLA, msgerror);
    	free(msgerror);
    	}
	    sumar_syscall(info_solicitud->file_descriptor);

	    connection_send(info_solicitud->file_descriptor, OC_RESP_ABRIR, &fd_proceso);

	}
    break;
	case OC_FUNCION_ESCRIBIR: {
	    sumar_syscall(info_solicitud->file_descriptor);
		int8_t *resp2 = malloc(sizeof(int8_t));
		escritura = malloc(sizeof(t_archivo));
		escritura = (t_archivo *) buffer;
		log_trace(logger, "Llamada a escritura. FD: %d.	informacion: %s", escritura->descriptor_archivo, (char*)escritura->informacion);
		if(escritura->descriptor_archivo == 1){
			//t_read_response * informacion_a_imprimir = obtener_informacion_a_imprimir(escritura->informacion, escritura->pid);
			//int socket_proceso = *(int*) dictionary_get(tabla_sockets_procesos, string_itoa(escritura->pid));

			int * _mismopid(t_par_socket_pid * target) {
				return escritura->pid == target->pid;
			}
			t_par_socket_pid * parEncontrado = (t_par_socket_pid*)list_find(tabla_sockets_procesos, _mismopid);
			if(parEncontrado){
				int socket_proceso = parEncontrado->socket;
				char * inf = malloc(strlen((char*)escritura->informacion));
				strcpy(inf, (char*)escritura->informacion);
				connection_send(socket_proceso, OC_ESCRIBIR_EN_CONSOLA, inf);
			}



			*resp2 = 1;

			connection_send(info_solicitud->file_descriptor, OC_RESP_ESCRIBIR, resp2);
		}else{
			t_table_file* tabla_archivos_proceso = getTablaArchivo(escritura->pid);
			t_process_file* file = buscarArchivoTablaProceso(tabla_archivos_proceso, escritura->descriptor_archivo);
			if(file==NULL){
				*resp2=EC_ARCHIVO_NO_ABIERTO;
				connection_send(info_solicitud->file_descriptor, OC_RESP_ESCRIBIR, resp2);
			}else{
				if(file->flags.escritura){
					 //char* path = getPath_Global(file->global_fd);
					rw_lock_unlock(LOCK_WRITE);
					t_global_file * global_file = getFileFromGlobal(file->global_fd);
					 char* path = global_file->file;
					 *resp2 = fs_write(fs_socket, path, file->offset_cursor, escritura->tamanio, escritura->tamanio, escritura->informacion, logger);
					 log_trace(logger, "se mandó a escribir a %s offset %d y la respuesta fue %d", path,file->offset_cursor,resp2);
					 rw_lock_unlock(UNLOCK);
					 switch(*resp2){
					 	 case SUCCESS:
					 		connection_send(info_solicitud->file_descriptor, OC_RESP_ESCRIBIR, resp2);
					 		break;
					 	 case ENOSPC:
					 		*resp2=EC_FS_LLENO;
					 		connection_send(info_solicitud->file_descriptor, OC_RESP_ESCRIBIR, resp2);
					 		break;
					 	 default:
					 		*resp2= EC_DESCONOCIDO;
					 		connection_send(info_solicitud->file_descriptor, OC_RESP_ESCRIBIR, resp2);
					 }
				}else{
					*resp2=EC_SIN_PERMISO_ESCRITURA;
					connection_send(info_solicitud->file_descriptor, OC_RESP_ESCRIBIR, resp2);
				}
			}
		}
		free(resp2);
		break;
	}
	case OC_FUNCION_LEER: {

	    sumar_syscall(info_solicitud->file_descriptor);
		t_pedido_archivo_leer * archivo_a_leer = (t_pedido_archivo_leer *)buffer;

		int leer_pagina = (archivo_a_leer->informacion)/TAMANIO_PAGINAS;
		int leer_offset = (archivo_a_leer->informacion) % TAMANIO_PAGINAS;

		t_table_file * tabla_encontrada = getTablaArchivo(archivo_a_leer->pid);
		t_process_file * process_file = buscarArchivoTablaProceso(tabla_encontrada, archivo_a_leer->descriptor_archivo);

		rw_lock_unlock(LOCK_READ);
		char * path = getPathFrom_PID_FD(archivo_a_leer->pid, archivo_a_leer->descriptor_archivo);
		t_fs_read_resp * read_response = fs_read(fs_socket, path, process_file->offset_cursor, archivo_a_leer->tamanio, logger);
		rw_lock_unlock(UNLOCK);


		if(read_response->exec_code != SUCCESS) {
			int _mismoPid(t_par_socket_pid *par) {
				return par->pid == pid;
			}
			char* msgerror= strdup("No se pudo leer el archivo");
			t_par_socket_pid * parNuevo = list_find(tabla_sockets_procesos, (void *) _mismoPid);
			if(parNuevo)connection_send(parNuevo->socket, OC_ESCRIBIR_EN_CONSOLA, msgerror);

			connection_send(info_solicitud->file_descriptor, OC_RESP_LEER_ERROR, &(read_response->exec_code));
		} else {

			int resultado = memory_write(memory_socket, archivo_a_leer->pid, leer_pagina, leer_offset, archivo_a_leer->tamanio, read_response->buffer_size, read_response->buffer, logger);

			connection_send(info_solicitud->file_descriptor, OC_RESP_LEER, &resultado);
		}
	}
	break;
	case OC_FUNCION_CERRAR: {

	    sumar_syscall(info_solicitud->file_descriptor);
		t_archivo * archivo = (t_archivo *)buffer;

		t_table_file* tabla_proceso = getTablaArchivo(archivo->pid);
		t_process_file* file = buscarArchivoTablaProceso(tabla_proceso, archivo->descriptor_archivo);
		if (file==NULL){
					*resp = EC_ARCHIVO_NO_ABIERTO;
					connection_send(info_solicitud->file_descriptor, OC_RESP_CERRAR, resp);
		}else{

			rw_lock_unlock(LOCK_WRITE);
			descontarDeLaTablaGlobal(file->global_fd);
			rw_lock_unlock(UNLOCK);

			bool _porFD(t_process_file* var){
				return var->proceso_fd == file->proceso_fd;
			}

			list_remove_and_destroy_by_condition(tabla_proceso->tabla_archivos, (void*) _porFD, free);
			*resp = EC_FINALIZACION_OK;
			connection_send(info_solicitud->file_descriptor, OC_RESP_CERRAR, resp);
		}
		break;
	}
	case OC_FUNCION_BORRAR: {
	    sumar_syscall(info_solicitud->file_descriptor);
		t_archivo * archivo = (t_archivo *)buffer;
		t_table_file* tabla_proceso = getTablaArchivo(archivo->pid);
		t_process_file * file = buscarArchivoTablaProceso(tabla_proceso, archivo->descriptor_archivo);
		if (file==NULL){
					*resp = EC_ARCHIVO_NO_ABIERTO;
					connection_send(info_solicitud->file_descriptor, OC_RESP_BORRAR, resp);
		}else{
			t_global_file * global = getFileFromGlobal(file->global_fd);

			if(global->open > 1) {
				*resp = EC_ARCHIVO_ABIERTO_OTROS;
				connection_send(info_solicitud->file_descriptor, OC_RESP_BORRAR, resp);
			} else {
				rw_lock_unlock(LOCK_WRITE);
				fs_delete_file(fs_socket, global->file, logger);
				//descontarDeLaTablaGlobal(file->global_fd);
				rw_lock_unlock(UNLOCK);

				bool _porFD(t_process_file* var){
					return var->proceso_fd == file->proceso_fd;
				}
				*resp = EC_FINALIZACION_OK;
				connection_send(info_solicitud->file_descriptor, OC_RESP_BORRAR, resp);
				//list_remove_and_destroy_by_condition(tabla_proceso->tabla_archivos, (void*) _porFD, free);
			}
		}

		break;
	}
	case OC_FUNCION_MOVER_CURSOR: {
		int pid;
	    sumar_syscall(info_solicitud->file_descriptor);
		t_valor_variable valor_variable;
		t_descriptor_archivo descriptor_archivo;

		//memcpy(&pid, buffer, sizeof(int));
		pid = *(uint8_t*)buffer;
		memcpy(&descriptor_archivo, buffer + sizeof(int), sizeof(t_descriptor_archivo));
		memcpy(&valor_variable, buffer + sizeof(int) + sizeof(t_descriptor_archivo), sizeof(t_valor_variable));

		t_table_file* tabla_proceso = getTablaArchivo(pid);
		t_process_file* file = buscarArchivoTablaProceso(tabla_proceso, descriptor_archivo);

		file->offset_cursor=valor_variable;
		log_trace(logger, "Mover cursor del archivo FD %d a la posicion %d", descriptor_archivo,file->offset_cursor);
	}
	break;
	case OC_FUNCION_ESCRIBIR_VARIABLE:
	    sumar_syscall(info_solicitud->file_descriptor);
		size_nombre = (int)*buffer;

		variable_recibida = malloc(sizeof(t_shared_var));
		variable_recibida->nombre = malloc( size_nombre*sizeof(char)+1);
		memcpy(variable_recibida->nombre, (void*)buffer+sizeof(int), size_nombre*sizeof(char));
		variable_recibida->nombre[size_nombre]='\0';
		memcpy(&(variable_recibida->valor), ((void*)buffer)+sizeof(int)+size_nombre*sizeof(char),sizeof(int));

		log_trace(logger, "pedido de asignar el valor %d a la variable %s", variable_recibida->valor,variable_recibida->nombre);
		asignarValorVariable(variable_recibida);
		free(variable_recibida->nombre);
		free(variable_recibida);

		break;
	case OC_FUNCION_LEER_VARIABLE:
	    sumar_syscall(info_solicitud->file_descriptor);
		nombre_variable	= (char*)buffer;
		log_trace(logger, "pedido de leer la variable %s",nombre_variable);
		t_valor_variable valor = leerValorVariable(nombre_variable);
		connection_send(info_solicitud->file_descriptor, OC_RESP_LEER_VARIABLE, &valor);
		break;
	case OC_FUNCION_SIGNAL:
		//TODO nombre_semaforo deberia venir en "buffer"
	    sumar_syscall(info_solicitud->file_descriptor);
		nombre_semaforo	= (char*)buffer;
		cpu = obtener_cpu(info_solicitud->file_descriptor);
		sem_wait(semSemaforos);
		semaforo = dictionary_get(semaforos, nombre_semaforo);
		semaforo->cuenta++;
		if(semaforo->cuenta <= 0){
			uint16_t* pid2 = queue_pop(semaforo->cola);
			uint16_t* pid = malloc(sizeof(uint16_t));
			memcpy(pid, pid2, sizeof(uint16_t)); //saco algun programa de la cola del semaforo
			log_trace(logger, "POP (%d) en Semaforo -%s- por CPU %d", *pid, nombre_semaforo, info_solicitud->file_descriptor);
			pasarDeBlockedAReady(*pid); //pasa el proceso a la cola de listos
			sem_post(semCantidadElementosColaListos);
			//cpu->proceso_desbloqueado_por_signal = 1;
			free(pid2);
			//pcb_destroy(auxPCB);
		}
		sem_post(semSemaforos);
		// Respuesta para desbloquear CPU, terminará la ejecución y devolverá el pcb par ser encolado en bloqueados
		*resp = 1;
		connection_send(info_solicitud->file_descriptor, OC_RESP_SIGNAL, resp);
		break;
	case OC_FUNCION_WAIT:
	    sumar_syscall(info_solicitud->file_descriptor);
		cpu = obtener_cpu(info_solicitud->file_descriptor);
		cpu->nombre_semaforo = (char*)buffer;
		cpu->proceso_bloqueado_por_wait = 1;

		// Respuesta para desbloquear CPU, terminará la ejecución y devolverá el pcb para ser encolado en bloqueados
		*resp = 1;
		connection_send(info_solicitud->file_descriptor, OC_RESP_WAIT, resp);
		break;
	case OC_TERMINA_PROGRAMA:
		pcb = deserializer_pcb(buffer);
		cpu = obtener_cpu(info_solicitud->file_descriptor);
		cpu->proceso_asignado = pcb;
		pthread_mutex_lock(&mutex_pedido_memoria);
		memory_finalize_process(memory_socket, pcb->pid, logger);
		pthread_mutex_unlock(&mutex_pedido_memoria);
		status = 1;
		int * _mismopid2(t_par_socket_pid * target) {
			return pcb->pid == target->pid;
		}
		t_par_socket_pid * parEncontrado2 = (t_par_socket_pid*)list_find(tabla_sockets_procesos, _mismopid2);
		connection_send(parEncontrado2->socket, OC_MUERE_PROGRAMA, &status);
		pasarDeExecuteAExit(cpu);
		sem_post(semCantidadCpuLibres);
		break;
	case OC_DESCONEX_CPU:
		pcb = deserializer_pcb(buffer);
		cpu = obtener_cpu(info_solicitud->file_descriptor);
		cpu->proceso_asignado = pcb;
		pasarDeExecuteAReady(cpu);
		eliminar_cpu(info_solicitud->file_descriptor);
		break;
	case OC_ERROR_EJECUCION_CPU:
		pcb = deserializer_pcb(buffer);
		if(pcb->exit_code != EC_ALOCAR_MUY_GRANDE && pcb->exit_code != EC_SIN_ESPACIO_MEMORIA){
			pthread_mutex_lock(&mutex_pedido_memoria);
			memory_finalize_process(memory_socket, pcb->pid, logger);
			pthread_mutex_unlock(&mutex_pedido_memoria);
		}

		cpu = obtener_cpu(info_solicitud->file_descriptor);
		cpu->proceso_asignado = pcb;
		status = 1;
		int * _mismopid(t_par_socket_pid * target) {
			return pcb->pid == target->pid;
		}
		t_par_socket_pid * parEncontrado = (t_par_socket_pid*)list_find(tabla_sockets_procesos, _mismopid);
		connection_send(parEncontrado->socket, OC_MUERE_PROGRAMA, &status);
		pasarDeExecuteAExit(cpu);
		sem_post(semCantidadCpuLibres);
		break;
	case OC_TERMINO_INSTRUCCION: {
		pthread_mutex_lock(&mutex_kill);
		t_PCB* oldPCB;
		*resp = -1; //por default no continua procesando
		pcb = deserializer_pcb(buffer);
		t_semaphore* semaforo;
		cpu = obtener_cpu(info_solicitud->file_descriptor);
		oldPCB = cpu->proceso_asignado;
		cpu->proceso_asignado = pcb;
		bool esta_bloqueado = false;
		bool liberarcpu = false;
		bool push_a_listos = false;

		if(cpu->proceso_bloqueado_por_wait){
			sem_wait(semSemaforos); // no se va a poder usar el semaforo hasta que no termine la instruccion (OC_TERMINO_INSTRUCCION)
			semaforo = dictionary_get(semaforos, cpu->nombre_semaforo);
			semaforo->cuenta--;
			if(semaforo->cuenta < 0){
				uint16_t* pid = malloc(sizeof(uint16_t));
				*pid = cpu->proceso_asignado->pid;
				queue_push(semaforo->cola, pid); //pongo el programa en la cola del semaforo
				log_trace(logger, "PUSH (%d) en Semaforo -%s- por CPU %d", *pid, cpu->nombre_semaforo, info_solicitud->file_descriptor);
				pasarDeExecuteABlocked(cpu); //bloquea el programa
				cpu->proceso_bloqueado_por_wait = 1;
			} else {
				cpu->proceso_bloqueado_por_wait = 0;
				sem_post(semSemaforos);
			}
		}

		if(cpu->matar_proceso){
			pcb->exit_code = -77;
			t_par_socket_pid* parEncontrado = encontrar_consola_de_pcb(pcb->pid);

			status = 1;
			if(parEncontrado){
				connection_send(parEncontrado->socket, OC_MUERE_PROGRAMA, &status); //TODO: Ver si se puede solucionar de otra forma para desconexion de consola
				FD_CLR(parEncontrado->socket, &master_prog);
			}
			pthread_mutex_lock(&mutex_pedido_memoria);
			memory_finalize_process(memory_socket, pcb->pid, logger);
			pthread_mutex_unlock(&mutex_pedido_memoria);
		} else {
			//esta_bloqueado = proceso_bloqueado(pcb);
			if( cpu->proceso_bloqueado_por_wait ){
				// si el proceso está bloqueado libera cpu
				liberarcpu = true;
			} else {
				if(continuar_procesando(cpu)){
					//se debe pasar el valor del sleep obtenido de la configuracion, esto ademas quiere decir que se debe continuar procesando
					*resp = kernel_conf->quantum_sleep;
				} else {
					pthread_mutex_lock(&semColaListos);
					queue_push(cola_listos, cpu->proceso_asignado);

					log_trace(logger, "PUSH (pid: %d - PC: %d - SP: %d POS %p) en Ready 488 soli.c", cpu->proceso_asignado->pid, cpu->proceso_asignado->PC, cpu->proceso_asignado->SP, cpu->proceso_asignado);

					log_trace(logger, "PID %d - Se inserta en Ready", cpu->proceso_asignado->pid);

					push_a_listos = true;
					liberarcpu = true;
				}
			}
		}
		if(!esta_bloqueado){
			//log_trace(logger, "PCB a destruir (pid: %d - PC: %d - SP: %d POS %p) ",  cpu->proceso_asignado->pid, cpu->proceso_asignado->PC, cpu->proceso_asignado->SP, cpu->proceso_asignado);
			//pcb_destroy(oldPCB);
		}
		//enviar oc para continuar ejecutando el proceso o no
		connection_send(info_solicitud->file_descriptor, OC_RESP_TERMINO_INSTRUCCION, resp);
		log_trace(logger, "Se envió OC_RESP_TERMINO_INSTRUCCION al socket %d", info_solicitud->file_descriptor);


		/*int cantidadProgramasListo_Sem;
		sem_getvalue(semCantidadElementosColaListos, &cantidadProgramasListo_Sem);
		sem_wait(semColaListos);
		int cantidadColaListos = queue_size(cola_listos);
		sem_post(semColaListos);
		if(cantidadColaListos>cantidadProgramasListo_Sem){*/
		if(push_a_listos){
			sem_post(semCantidadElementosColaListos);
			pthread_mutex_unlock(&semColaListos);
		}

		if(cpu->proceso_bloqueado_por_wait){
			//cpu->proceso_bloqueado_por_wait = 0;
			sem_post(semSemaforos); // libero el semaforo para que pueda usarse en el signal
		}

		if (liberarcpu) {
			liberar_cpu(cpu);
			sem_post(semCantidadCpuLibres);
		} else {
			if(cpu->matar_proceso){
				pasarDeExecuteAExit(cpu);
				sem_post(semCantidadCpuLibres);
			}
		}
		pthread_mutex_unlock(&mutex_kill);
	}

		break;

	case OC_KILL_CONSOLA: {
		pid = *(int*)buffer;
		status = 1;
		if(pasar_proceso_a_exit(pid)){
			t_par_socket_pid* parEncontrado = encontrar_consola_de_pcb(pid);
			int status = 1;
			connection_send(parEncontrado->socket, MUERE_PROGRAMA, &status);
		}

	break;
	}
	default:
		fprintf(stderr, "Desconexion\n");
		return;
	}
	free(resp);
}

int calcular_paginas_de_codigo(char* codigo){
	int tamanio_codigo, paginas;
	tamanio_codigo = strlen(codigo);
	paginas = tamanio_codigo / TAMANIO_PAGINAS;
	if((paginas * TAMANIO_PAGINAS) < tamanio_codigo) return ++paginas;
	return paginas;
}

int calcular_paginas_necesarias(char* codigo){
	int paginas_de_codigo = calcular_paginas_de_codigo(codigo);
	return paginas_de_codigo + kernel_conf->stack_size;
}


void mandar_codigo_a_memoria(char* codigo, int pid){
	int i = 0, offset = 0, cant_a_mandar = strlen(codigo);
	while(cant_a_mandar > TAMANIO_PAGINAS){
		pthread_mutex_lock(&mutex_pedido_memoria);
		memory_write(memory_socket, pid, i, 0, TAMANIO_PAGINAS, TAMANIO_PAGINAS, codigo + offset, logger);
		pthread_mutex_unlock(&mutex_pedido_memoria);
		offset += TAMANIO_PAGINAS;
		cant_a_mandar -= TAMANIO_PAGINAS;
		i++;
	}
	if(cant_a_mandar > 0){
		pthread_mutex_lock(&mutex_pedido_memoria);
		memory_write(memory_socket, pid, i, 0, cant_a_mandar, cant_a_mandar, codigo + offset, logger);
		pthread_mutex_unlock(&mutex_pedido_memoria);
	}
}

t_indice_codigo* obtener_indice_codigo(t_metadata_program* metadata){
	int i = 0;
	log_trace(logger, "Dentro de obtener_indice_codigo");
	t_indice_codigo* indice_codigo = malloc(sizeof(t_indice_codigo) * metadata->instrucciones_size);
	for(i = 0; i < metadata->instrucciones_size; i++){
		memcpy((indice_codigo + i), (metadata->instrucciones_serializado )+ i, sizeof(t_indice_codigo));
		log_trace(logger, "Instrucción nro %d: offset %d, size %d", i,(indice_codigo + i)->offset, (indice_codigo + i)->size);
	}
	return indice_codigo;
}

t_dictionary* obtener_indice_etiquetas(t_metadata_program* metadata){
	t_dictionary* indice_etiquetas = dictionary_create();
	char* key;
	int *value, offset = 0;

	int i, cantidad_etiquetas_total = metadata->cantidad_de_etiquetas + metadata->cantidad_de_funciones;	// cantidad de tokens que espero sacar del bloque de bytes
	for(i=0; i < cantidad_etiquetas_total; i++){
		value = malloc(sizeof(t_puntero_instruccion));
		int cant_letras_token = 0;
		while(metadata->etiquetas[cant_letras_token + offset] != '\0')cant_letras_token++;
		key = malloc(cant_letras_token + 1);
		memcpy(key, metadata->etiquetas + offset, cant_letras_token + 1);		// copio los bytes de metadata->etiquetas desplazado las palabras que ya copie
		offset += cant_letras_token + 1;										// el offset suma el largo de la palabra + '\0'
		memcpy(value, metadata->etiquetas+offset,sizeof(t_puntero_instruccion));// copio el puntero de instruccion
		offset += sizeof(t_puntero_instruccion);
		dictionary_put(indice_etiquetas, key, value);
	}
	return indice_etiquetas;
}

t_pagina_heap* obtener_pagina_con_suficiente_espacio(int pid, int espacio){
	bool tiene_mismo_pid_y_espacio_disponible(t_pagina_heap* pagina){
		return (pagina->pid == pid && pagina->espacio_libre >= (espacio + sizeof(t_heapMetadata)));
	}
	return list_find(tabla_paginas_heap, (void*)tiene_mismo_pid_y_espacio_disponible);
}

t_pagina_heap* crear_pagina_heap(int pid, int nro_pagina){
	t_pagina_heap* new = malloc(sizeof(t_pagina_heap));
	new->pid = pid;
	new->nro_pagina = nro_pagina;
	new->espacio_libre = TAMANIO_PAGINAS - sizeof(t_heapMetadata);
	return new;
}

void tabla_heap_agregar_pagina(int pid){
	bool _mismo_pid(t_pagina_heap* p){
		return p->pid == pid;
	}
	int ultimo_nro_pag;
	t_list* filtrados = list_filter(tabla_paginas_heap, (void*) _mismo_pid);
	t_pagina_heap* ultima_pagina = list_get(filtrados, list_size(filtrados) - 1);
	if(list_size(filtrados) == 0){
		ultimo_nro_pag = -1;
	} else {
		ultimo_nro_pag = ultima_pagina->nro_pagina;
	}
	t_pagina_heap* nueva_pag = crear_pagina_heap(pid, ultimo_nro_pag + 1);
	list_add(tabla_paginas_heap, nueva_pag);
}

t_heapMetadata* leer_metadata(void* pagina){
	t_heapMetadata* new = malloc(sizeof(t_heapMetadata));
	memcpy(&(new->size), pagina, sizeof(uint32_t));
	memcpy(&(new->isFree), ((char*)pagina) + sizeof(uint32_t), sizeof(bool));
	return new;
}

t_puntero buscar_bloque_disponible(void* pagina, int espacio_pedido){			// Devuelve el puntero a la metadata del bloque con espacio suficiente
	t_heapMetadata* metadata;
	t_puntero posicion_bloque;
	int espacio_total_bloque;
	t_puntero offset = 0;
	while(offset < TAMANIO_PAGINAS){
		metadata = leer_metadata(pagina + offset);
		if((metadata->isFree) && metadata->size >= (espacio_pedido + sizeof(t_heapMetadata))){		// hay un bloque con suficiente espacio libre
			free(metadata);
			return offset;
		}
		offset += sizeof(t_heapMetadata);			//avanza la cantidad de bytes de la metadata
		offset += metadata->size;					//avanza la cantidad de bytes del bloque de datos
	}
	return NULL;
}

void cambiar_metadata(t_heapMetadata* metadata, int espacio_pedido){
	metadata->isFree = 0;
	metadata->size = espacio_pedido;

}

void agregar_bloque_libre(char* pagina, int offset){
	int espacio_libre;
	espacio_libre = TAMANIO_PAGINAS - offset;
	t_heapMetadata* metadata_libre = crear_metadata_libre(espacio_libre);
	memcpy(pagina + offset, metadata_libre, sizeof(t_heapMetadata));
	free(metadata_libre);
}

t_pagina_heap* buscar_pagina_heap(int pid, int nro_pagina){
	bool _pagina_de_programa(t_pagina_heap* pagina){
		return (pagina->pid == pid && pagina->nro_pagina == nro_pagina);
	}
	return list_find(tabla_paginas_heap, (void*) _pagina_de_programa);
}

void marcar_bloque_libre(t_heapMetadata* metadata, char* pagina){
	metadata->isFree = 1;
	memcpy(pagina, metadata, sizeof(t_heapMetadata));
}

void tabla_heap_cambiar_espacio_libre(t_pedido_liberar_memoria* pedido_free, int espacio_liberado){
	t_pagina_heap* pagina_de_tabla = buscar_pagina_heap(pedido_free->pid, pedido_free->nro_pagina);
	pagina_de_tabla->espacio_libre += espacio_liberado;
}

t_heapMetadata* crear_metadata_libre(uint32_t espacio){
	t_heapMetadata* new = malloc(sizeof(t_heapMetadata));
	new->isFree = 1;
	new->size = espacio - sizeof(t_heapMetadata);
	return new;
}

void marcar_bloque_ocupado(t_puntero bloque_heap_ptr, char* pagina, int espacio_pedido){
	t_heapMetadata* metadata = leer_metadata(pagina + bloque_heap_ptr);
	t_heapMetadata* metadata2;
	int sobrante = metadata->size - espacio_pedido;
	metadata->isFree = 0;
	metadata->size = espacio_pedido;
	memcpy(pagina + bloque_heap_ptr, metadata, sizeof(t_heapMetadata));
	metadata2 = crear_metadata_libre(sobrante);
	memcpy(pagina + bloque_heap_ptr + sizeof(t_heapMetadata) + metadata->size, metadata2, sizeof(t_heapMetadata));
	free(metadata);
	free(metadata2);
}

void modificar_pagina(t_pagina_heap* pagina, int espacio_ocupado){
	pagina->espacio_libre = pagina->espacio_libre - (espacio_ocupado + sizeof(t_heapMetadata));
}

int abrir_archivo(uint16_t pid, char* direccion, t_banderas flags){
	int fd_proceso;
	int fd_global; //guarda la posición del archivo en la tabla global
	rw_lock_unlock(LOCK_READ);
	fd_global = buscarArchivoTablaGlobal(direccion);
	rw_lock_unlock(UNLOCK);
	int result = fs_validate_file(fs_socket, direccion, logger);

	if(fd_global == -1) {
		if((flags.lectura || flags.escritura) && result == ISREG ) {
			rw_lock_unlock(LOCK_WRITE);
			fd_global = crearArchivoTablaGlobal(direccion);
			rw_lock_unlock(UNLOCK);
			fd_proceso = cargarArchivoTablaProceso(pid, fd_global, flags);

		} else if(flags.creacion) {  //&& result == ISNOTREG
			int res = fs_create_file(fs_socket, direccion, logger);
			if(res>0){
				rw_lock_unlock(LOCK_WRITE);
				fd_global = crearArchivoTablaGlobal(direccion);
				rw_lock_unlock(UNLOCK);
				fd_proceso = cargarArchivoTablaProceso(pid, fd_global, flags);
			}else{
				fd_proceso = res;
			}

		} else {
			fd_global = -1;
			fd_proceso = -1;
		}

	} else {
		fd_proceso = cargarArchivoTablaProceso(pid, fd_global, flags);
	}

	return fd_proceso;

}

int crearArchivoTablaGlobal(char* direccion){
	t_global_file * filereg = malloc(sizeof(t_global_file));
	filereg->file = malloc(string_length(direccion)+1);

	memcpy(filereg->file, direccion,string_length(direccion)+1);
	filereg->global_fd = contador_fd_global++;
	filereg->open=1;

	list_add(tabla_global_archivos,filereg);

	return filereg->global_fd;
}


int buscarArchivoTablaGlobal(char* direccion){

	t_global_file * filereg;
	bool encontro = false;

	int i;
	for (i=0;i<list_size(tabla_global_archivos);i++){

		filereg = list_get(tabla_global_archivos,i);

		if(strcmp(filereg->file, direccion)==0){
			encontro = true;
			filereg->open++;
			break;
		}
	}
	if(!encontro) {
		return -1;
	} else {
		return filereg->global_fd;
	}

}

t_process_file* buscarArchivoTablaProceso(t_table_file* tabla, int fd_archivo){

	t_process_file* file;
	bool _porPID(t_process_file* var){
		return var->proceso_fd == fd_archivo;
	}
	file = list_find(tabla->tabla_archivos,(void*) _porPID);
	return file;
}
//char* getPath_Global(int fdGlobal){
//	t_global_file * filereg = getFileFromGlobal(fdGlobal);
//	return filereg->file;
//}

t_global_file * getFileFromGlobal(int global_fd) {
	t_global_file * filereg;

	int _ByFDGlobal(t_global_file * gf) {
		return gf->global_fd == global_fd;
	}
	filereg = list_find(tabla_global_archivos, (void*) _ByFDGlobal);
	return filereg;
}

void descontarDeLaTablaGlobal(int global_fd) {

	void _destroy_globalfile(t_global_file * global_file) {
		free(global_file->file);
		free(global_file);
	}

	int _byFD(t_global_file * global_file) {
		return global_file->global_fd == global_fd;
	}

	t_global_file * filereg = getFileFromGlobal(global_fd);

	if(filereg->open > 1) {
		filereg->open--;
	}
	else {
		list_remove_and_destroy_by_condition(tabla_global_archivos, (void *) _byFD, (void *) _destroy_globalfile);
	}

}

char* getPathFrom_PID_FD(int pid, int fdProceso){
	t_table_file* tabla_archivos_proceso = getTablaArchivo(pid);
	t_process_file* file = buscarArchivoTablaProceso(tabla_archivos_proceso, fdProceso);

	if(file == NULL) {
		return -1;
	}
	t_global_file * global_file = getFileFromGlobal(file->global_fd);
	char* path = global_file->file;
	return path;
}

int cargarArchivoTablaProceso(int pid, int fd_global, t_banderas flags){
	t_process_file* file = malloc(sizeof(t_process_file));
	t_table_file * tablaDeArchivosDeProceso = getTablaArchivo(pid);

	file->global_fd = fd_global;
	file->proceso_fd = tablaDeArchivosDeProceso->contador_fd++;
	file->flags = flags;
	file->offset_cursor = 0;

	list_add(tablaDeArchivosDeProceso->tabla_archivos, file);

	return file->proceso_fd;
}



t_list* crearTablaArchProceso(){
	t_list* tabla_archivo_proceso = list_create();

	return tabla_archivo_proceso;
}

void defragmentar(char* pagina, t_pedido_liberar_memoria* pedido_free){
	int offset = 0;
	t_pagina_heap* pag_heap;
	t_heapMetadata* metadata, *metadata2;
	metadata = leer_metadata(pagina);
	offset += sizeof(t_heapMetadata);
	offset += metadata->size;
	while(offset < TAMANIO_PAGINAS){
		metadata2 = leer_metadata(pagina + offset);
		if(metadata->isFree && metadata2->isFree){
			juntar_bloques(metadata, metadata2);
			memcpy(pagina + offset - sizeof(t_heapMetadata) - metadata->size, metadata2, sizeof(t_heapMetadata));
			tabla_heap_cambiar_espacio_libre(pedido_free, sizeof(t_heapMetadata));
			break;
		}
		metadata = metadata2;
		offset += sizeof(t_heapMetadata);
		offset += metadata2->size;
	}
}

void juntar_bloques(t_heapMetadata* metadata1, t_heapMetadata* metadata2){
	metadata2->size += sizeof(t_heapMetadata) + metadata1->size;
}

void tabla_heap_sacar_pagina(t_pedido_liberar_memoria* pedido_free){		// TODO PREGUNTAR a gus si descubrio como sacar elemento de lista
	bool _pagina_de_programa(t_pagina_heap* pagina){
		return (pagina->pid == pedido_free->pid && pagina->nro_pagina == pedido_free->nro_pagina);
	}
	list_remove_by_condition(tabla_paginas_heap, (void*) _pagina_de_programa);
}

void liberar_pagina(t_pedido_liberar_memoria* pedido_free, int offset_de_paginas){
	memory_delete_page(memory_socket, pedido_free->pid, pedido_free->nro_pagina + offset_de_paginas, logger);
}

bool pagina_vacia(int pid, int nro_pagina){
	t_pagina_heap* pag = buscar_pagina_heap(pid, nro_pagina);
	return (pag->espacio_libre == (TAMANIO_PAGINAS - sizeof(t_heapMetadata)));
}

t_codigo_proceso* buscar_codigo_de_proceso(int pid){
	bool _mismo_pid(t_codigo_proceso* c){
		return c->pid == pid;
	}
	return list_find(tabla_paginas_por_proceso, (void*) _mismo_pid);
}

void asignarValorVariable(t_shared_var* variable_recibida){
	bool _porNombreVarComp(t_shared_var* var){
		return strcmp(var->nombre,variable_recibida->nombre)==0;
	}
	t_shared_var* variable = list_find(tabla_variables_compartidas,(void*) _porNombreVarComp);
	variable->valor = variable_recibida->valor;
}

t_valor_variable leerValorVariable(char* nombre_variable){
	bool _porNombreVarComp(t_shared_var* var){
		return strcmp(var->nombre,nombre_variable)==0;
	}
	t_shared_var* variable = list_find(tabla_variables_compartidas,(void*) _porNombreVarComp);
	return variable->valor;
}

t_read_response * obtener_informacion_a_imprimir(t_puntero puntero, int pid) {

	int pagina = puntero/TAMANIO_PAGINAS;
	int offset = puntero % TAMANIO_PAGINAS;

	return memory_read(memory_socket, pid, pagina, offset, sizeof(t_puntero), logger);
}

void obtener_direccion_relativa(t_puntero* puntero, int nro_pagina_heap, int cantidad_paginas_codigo){
	*puntero += sizeof(t_heapMetadata);
	*puntero += TAMANIO_PAGINAS * nro_pagina_heap;
	*puntero += TAMANIO_PAGINAS * cantidad_paginas_codigo;
}

void obtener_direccion_de_bloque_verdadera(t_pedido_liberar_memoria* pedido_free, int cantidad_paginas_codigo){

	pedido_free->nro_pagina =(pedido_free->posicion / TAMANIO_PAGINAS) - cantidad_paginas_codigo;
	pedido_free->posicion = pedido_free->posicion % TAMANIO_PAGINAS;
	pedido_free->posicion -= sizeof(t_heapMetadata);

}

void sumar_syscall(int socket){
	t_cpu* cpu = obtener_cpu(socket);

	int * _mismopid(t_par_socket_pid * target) {
		return cpu->proceso_asignado->pid == target->pid;
	}
	t_par_socket_pid * parEncontrado = (t_par_socket_pid*)list_find(tabla_sockets_procesos, _mismopid);
	if(parEncontrado)parEncontrado->cantidad_syscalls++;
}

void sumar_espacio_reservado(int espacio_pedido, int socket){
	t_cpu* cpu = obtener_cpu(socket);
	int * _mismopid(t_par_socket_pid * target) {
		return cpu->proceso_asignado->pid == target->pid;
	}
	t_par_socket_pid * parEncontrado = (t_par_socket_pid*)list_find(tabla_sockets_procesos, _mismopid);
	if(parEncontrado)parEncontrado->memoria_reservada += espacio_pedido;
}

void	sumar_espacio_liberado(int pid, int espacio_liberado){
	t_cpu* cpu = obtener_cpu_por_proceso(pid);
	int * _mismopid(t_par_socket_pid * target) {
		return cpu->proceso_asignado->pid == target->pid;
	}
	t_par_socket_pid * parEncontrado = (t_par_socket_pid*)list_find(tabla_sockets_procesos, _mismopid);
	if(parEncontrado)parEncontrado->memoria_liberada += espacio_liberado;
}

int notificar_memoria_inicio_programa(int pid, int cant_paginas, char* codigo_completo){
	int status = 0;
	pthread_mutex_lock(&mutex_pedido_memoria);
	status = memory_init_process(memory_socket, pid, cant_paginas, logger);
	pthread_mutex_unlock(&mutex_pedido_memoria);
	if(status == -203){
		log_error(logger, "No hay espacio suficiente para nuevo programa");
		return status;
	}
	mandar_codigo_a_memoria(codigo_completo, pid);
	return status;
}

t_par_socket_pid* encontrar_consola_de_pcb(int pid){
	int * _mismopid(t_par_socket_pid * target) {
		return pid == target->pid;
	}
	return list_find(tabla_sockets_procesos, _mismopid);
}
