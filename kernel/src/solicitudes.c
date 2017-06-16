/*
 * solicitudes.c
 *
 *  Created on: 15/5/2017
 *      Author: utnso
 */

#include "solicitudes.h"
#include <parser/metadata_program.h>
#include <parser/parser.h>

void solve_request(int socket, fd_set* set){
	uint8_t operation_code;
	uint32_t cant_paginas, direcc_logica, direcc_fisica;
	t_puntero bloque_heap_ptr;
	int status;
	char* buffer;
	t_pedido_reservar_memoria* pedido;
	t_pedido_liberar_memoria* liberar;
	t_pagina_heap* pagina;
	t_read_response* respuesta_pedido_pagina;
	t_PCB* pcb;
	t_metadata_program* metadata;
	status = connection_recv(socket, &operation_code, &buffer);
	if(status <= 0)	FD_CLR(socket, set);

	switch(operation_code){
	case OC_SOLICITUD_PROGRAMA_NUEVO:

		cant_paginas = calcular_paginas_necesarias(buffer);
		pcb = crear_PCB();
		status = 0;
		status = memory_init_process(memory_socket, pcb->pid, cant_paginas, logger);

		if(status == -1){
			log_error(logger, "Se desconecto Memoria");
			exit(1);
		}

		mandar_codigo_a_memoria(buffer, pcb->pid);
		pcb->cantidad_paginas += cant_paginas;
		pcb->PC = 0;


		connection_send(socket, OC_NUEVA_CONSOLA_PID, &(pcb->pid));

		metadata = metadata_desde_literal(buffer);

		pcb->SP = 0;
		pcb->cantidad_instrucciones = metadata->instrucciones_size;
		pcb->indice_codigo = obtener_indice_codigo(metadata);
		pcb->indice_etiquetas = obtener_indice_etiquetas(metadata);
		metadata_destruir(metadata);
		break;
	case OC_FUNCION_RESERVAR:
		pedido = buffer;
		pagina = obtener_pagina_con_suficiente_espacio(pedido->pid, pedido->espacio_pedido);
		if(pagina == NULL){
			memory_assign_pages(memory_socket, pedido->pid, 1, logger);

			tabla_heap_agregar_pagina(pedido->pid);
			pagina = obtener_pagina_con_suficiente_espacio(pedido->pid, pedido->espacio_pedido);

			t_heapMetadata* meta_pag_nueva =crear_metadata_libre(TAMANIO_PAGINAS);

			// Escribimos la metadata de la nueva pagina en Memoria
			memory_write(memory_socket, pedido->pid, pagina->nro_pagina, 0, sizeof(t_heapMetadata), sizeof(t_heapMetadata), meta_pag_nueva, logger);

			free(meta_pag_nueva);
		}
		respuesta_pedido_pagina = memory_read(memory_socket, pedido->pid, pagina->nro_pagina, 0, TAMANIO_PAGINAS, logger);
		bloque_heap_ptr = buscar_bloque_disponible(respuesta_pedido_pagina->buffer, pedido->espacio_pedido);

		marcar_bloque_ocupado(bloque_heap_ptr, pagina, pedido->espacio_pedido);
		// Mandamos la pagina de heap modificada
		memory_write(memory_socket, pedido->pid, pagina->nro_pagina, 0, TAMANIO_PAGINAS, TAMANIO_PAGINAS, respuesta_pedido_pagina->buffer, logger);

		modificar_pagina(pagina, pedido->espacio_pedido);
		// Mandamos puntero al programa que lo pidio
		bloque_heap_ptr += TAMANIO_PAGINAS * pagina->nro_pagina;
		connection_send(socket, OC_RESP_RESERVAR, bloque_heap_ptr);

		break;
	case OC_FUNCION_LIBERAR:
		liberar = buffer;
		liberar->nro_pagina = liberar->posicion / TAMANIO_PAGINAS;
		respuesta_pedido_pagina = memory_read(memory_socket, liberar->pid, liberar->nro_pagina, 0, TAMANIO_PAGINAS, logger);
		marcar_bloque_libre(respuesta_pedido_pagina->buffer, liberar);
		defragmentar(respuesta_pedido_pagina->buffer, liberar);
		memory_write(memory_socket, liberar->pid, liberar->nro_pagina, 0, TAMANIO_PAGINAS, TAMANIO_PAGINAS, respuesta_pedido_pagina->buffer, logger);
	}
}

int calcular_paginas_de_codigo(char* codigo){
	int tamanio_codigo, paginas;
	tamanio_codigo = strlen(codigo);
	paginas = tamanio_codigo / TAMANIO_PAGINAS;
	if((paginas * TAMANIO_PAGINAS) < tamanio_codigo) return paginas++;
	return paginas;
}

int calcular_paginas_necesarias(char* codigo){
	int paginas_de_codigo = calcular_paginas_de_codigo(codigo);
	return paginas_de_codigo + kernel_conf->stack_size;
}


void mandar_codigo_a_memoria(char* codigo, int pid){
	int i = 0, offset = 0, cant_a_mandar = strlen(codigo);
	while(cant_a_mandar > TAMANIO_PAGINAS){
		memory_write(memory_socket, pid, i, 0, TAMANIO_PAGINAS, TAMANIO_PAGINAS, codigo + offset, logger);
		offset += TAMANIO_PAGINAS;
		cant_a_mandar -= TAMANIO_PAGINAS;
		i++;
	}
	if(cant_a_mandar > 0){
		memory_write(memory_socket, pid, i, 0, cant_a_mandar, cant_a_mandar, codigo + offset, logger);
	}
}


t_indice_codigo* obtener_indice_codigo(t_metadata_program* metadata){
	int i = 0;
	t_indice_codigo* indice_codigo = malloc(sizeof(t_indice_codigo) * metadata->instrucciones_size);
	for(i = 0; i < metadata->instrucciones_size; i++){
		memcpy((indice_codigo + i), (metadata->instrucciones_serializado )+ i, sizeof(t_indice_codigo));
	}
	return indice_codigo;
}

t_dictionary* obtener_indice_etiquetas(t_metadata_program* metadata){
	t_dictionary* indice_etiquetas = dictionary_create();
	char* key;
	int *value, offset = 0;
	value = malloc(sizeof(t_puntero_instruccion));
	int i, cantidad_etiquetas_total = metadata->cantidad_de_etiquetas + metadata->cantidad_de_funciones;	// cantidad de tokens que espero sacar del bloque de bytes
	for(i=0; i < cantidad_etiquetas_total; i++){
		int cant_letras_token = 0;
		while(metadata->etiquetas[cant_letras_token + offset] != '\0')cant_letras_token++;
		key = malloc(cant_letras_token + 1);
		memcpy(key, metadata->etiquetas + offset, cant_letras_token + 1);		// copio los bytes de metadata->etiquetas desplazado las palabras que ya copie
		offset += cant_letras_token + 1;										// el offset suma el largo de la palabra + '\0'
		memcpy(value, metadata->etiquetas+offset,sizeof(t_puntero_instruccion));// copio el puntero de instruccion
		offset += sizeof(t_puntero_instruccion);
		dictionary_put(indice_etiquetas, key, *value);
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
	t_list* filtrados = list_filter(tabla_paginas_heap, (void*) _mismo_pid);
	int ultima_pagina = list_get(filtrados, list_size(filtrados) - 1);
	t_pagina_heap* nueva_pag = crear_pagina_heap(pid, ultima_pagina + 1);
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
		if(!(metadata->isFree) && metadata->size >= (espacio_pedido + sizeof(t_heapMetadata))){		// hay un bloque con suficiente espacio libre
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

void marcar_bloque_libre(char* pagina, t_pedido_liberar_memoria* pedido_free){
	int offset = pedido_free->posicion % TAMANIO_PAGINAS;
	t_heapMetadata* metadata = leer_metadata(pagina + offset);
	metadata->isFree = 1;
	memcpy(pagina + offset, metadata, sizeof(t_heapMetadata));

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
