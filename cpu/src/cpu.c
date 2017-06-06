/*
 ============================================================================
 Name        : cpu.c
 Author      : Carlos Flores
 Version     :
 Copyright   : GitHub @Charlos
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <commons/config.h>
#include <commons/collections/list.h>
#include <shared-library/socket.h>
#include <parser/metadata_program.h>
#include "cpu.h"
#include "funcionesCPU.h"

AnSISOP_funciones * funciones;
AnSISOP_kernel * func_kernel;
t_cpu_conf* cpu_conf;
t_log* logger;
int pagesize;

int stackPointer;
t_page_offset* lastPageOffset;
t_PCB* pcb;

int server_socket_kernel, server_socket_memoria;

void procesarMsg(char * msg);

int main(void) {

	//int byte_ejecutados

	crear_logger("/home/utnso/workspace/tp-2017-1c-Stranger-Code/cpu/cpu", &logger, true, LOG_LEVEL_TRACE);
	log_trace(logger, "Log Creado!!");

	load_properties();
	server_socket_kernel = connect_to_socket(cpu_conf->kernel_ip, cpu_conf->kernel_port);
	server_socket_memoria = connect_to_socket(cpu_conf->memory_ip, cpu_conf->memory_port);
	inicializarFuncionesParser();
	pagesize = handshake(server_socket_memoria, logger);

	if (pagesize>0){
		log_trace(logger, "Handshake con Memoria. El tamaño de la página es %d",pagesize);
	} else {
		log_trace(logger, "Problema con Handshake con Memoria.");
	}

	/*while(1) {
		pcb = malloc(sizeof(t_PCB));
		uint8_t operation_code;

		connection_recv(server_socket_kernel, &operation_code, pcb);

		int pc, page, cantInstrucciones;

		cantInstrucciones = pcb->cantidad_paginas;
		lastPageOffset = malloc(sizeof(lastPageOffset));

		loadlastPosStack();

		if(list_size(pcb->indice_stack) == 0) {

			//Si el indice del stack está vacio es porque estamos en la primera línea de código, creo la primera línea del scope
			nuevoContexto();
		}

		t_indice_codigo * icodigo = malloc(sizeof(t_indice_codigo));
		icodigo = ((t_indice_codigo*) pcb->indice_codigo)+pc;

		//TODO ver de cambiar la esctructura indice de codigo
		page = calcularPagina();

		//pido leer la instruccion a la memoria
		t_read_response * read_response = memory_read(server_socket_memoria, pcb->pid, page, icodigo->offset, icodigo->size, logger);

		char * instruccion;
		strcpy(instruccion, read_response->buffer);

		procesarMsg(instruccion);

		free(instruccion);
		free(read_response->buffer);
		free(read_response);
		free(icodigo);

		pcb->PC++;

		connection_send(server_socket_kernel, OC_PCB, pcb);
	}*/

	//TODO: loop de esto y dentro del loop el reciv para quedar a la espera de que kernel nos envíe un pcb
	// una vez que recibimos procesamos una línea, devolvemos el pbc y quedamos a la espera de recibir el proximo
	pcb = malloc(sizeof(t_PCB));
	uint8_t operation_code;
	connection_recv(server_socket_kernel, &operation_code, pcb);

	int pc, page, cantInstrucciones;
	//TODO cambiar este valor fijo por el que recibamos durante la recepción del PCB
	cantInstrucciones = 10;
	lastPageOffset= malloc(sizeof(lastPageOffset));
	loadlastPosStack();

	//Se incrementa Program Counter para comenzar la ejecución
	pcb->PC++;

	for( pc = pcb->PC ; pc <= cantInstrucciones ; pc++){

		if(list_size(pcb->indice_stack)==0){
			//Si el indice del stack está vacio es porque estamos en la primera línea de código, creo la primera línea del scope
			nuevoContexto();
		}
		t_indice_codigo* icodigo = malloc(sizeof(t_indice_codigo));
		icodigo = ((t_indice_codigo*) pcb->indice_codigo)+pc;
//TODO ver de cambiar la esctructura indice de codigo
		page = calcularPagina();

		//pido leer la instruccion a la memoria
		t_read_response * read_response = memory_read(server_socket_memoria, pcb->pid, page, icodigo->offset, icodigo->size, logger);

		char * instruccion;
		strcpy(instruccion, read_response->buffer);

		procesarMsg(instruccion);

		free(instruccion);
		free(read_response->buffer);
		free(read_response);
		free(icodigo);

	}

	return EXIT_SUCCESS;

}
