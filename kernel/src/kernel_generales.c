/*
 * kernel_generales.c
 *
 *  Created on: 17/5/2017
 *      Author: utnso
 */

#include "kernel_generales.h"


void load_kernel_properties(void) {
	t_config * conf = config_create("/home/utnso/workspace/tp-2017-1c-Stranger-Code/kernel/Debug/kernel.cfg");
	kernel_conf = malloc(sizeof(t_kernel_conf));
	kernel_conf->program_port = config_get_int_value(conf, "PUERTO_PROG");
	kernel_conf->cpu_port = config_get_int_value(conf, "PUERTO_CPU");
	kernel_conf->memory_ip = config_get_string_value(conf, "IP_MEMORIA");
	kernel_conf->memory_port = config_get_string_value(conf, "PUERTO_MEMORIA");
	kernel_conf->filesystem_ip = config_get_string_value(conf, "IP_FS");
	kernel_conf->filesystem_port = config_get_string_value(conf, "PUERTO_FS");
	kernel_conf->grado_multiprog = config_get_int_value(conf, "GRADO_MULTIPROG");
	kernel_conf->algoritmo = config_get_string_value(conf, "ALGORITMO");
	kernel_conf->quantum = config_get_int_value(conf, "QUANTUM");
	kernel_conf->quantum_sleep = config_get_int_value(conf, "QUANTUM_SLEEP");
	kernel_conf->stack_size = config_get_int_value(conf, "STACK_SIZE");
	kernel_conf->sem_ids = config_get_string_value(conf, "SEM_IDS");
	kernel_conf->sem_init = config_get_array_value(conf, "SEM_INIT");
	kernel_conf->shared_vars = config_get_string_value(conf, "SHARED_VARS");

	crearVariablesCompartidas();
	crearSemaforos();
}

t_PCB* crear_PCB(){
	t_PCB* PCB = malloc(sizeof(t_PCB));
	PCB->pid = registro_pid++;
	PCB->cantidad_paginas = 0;
	return PCB;
}

void crearVariablesCompartidas(){
	int i=0;

    int length_value = strlen(kernel_conf->shared_vars) - 2;
    char* value_without_brackets = string_substring(kernel_conf->shared_vars, 1, length_value);

	char** variables = string_split(value_without_brackets, ",");
	while(variables[i]!=NULL){
		t_shared_var* variable = malloc(sizeof(t_shared_var));
		string_trim(&(variables[i]));
		char* nombre = string_substring(variables[i], 1, strlen(variables[i]) - 1);
		variable->nombre=nombre;
		variable->valor=0;
		list_add(tabla_variables_compartidas,variable);
		i++;
	}
}

t_cpu* obtener_cpu(socket){
	return find_by_fd(socket);
}

t_cpu* find_by_fd(int fd) {
	int _is_fd(t_cpu *cpu) {
		return (cpu->file_descriptor == fd);
	}

	return list_find(lista_cpu, (void*) _is_fd);
}

void crearSemaforos(){
	int i=0;
	semaforos = dictionary_create();
    int length_value = strlen(kernel_conf->sem_ids) - 2;
    char* value_without_brackets = string_substring(kernel_conf->sem_ids, 1, length_value);

	char** semaforos_cfg = string_split(value_without_brackets, ",");
	while(semaforos_cfg[i]!=NULL){
		string_trim(&(semaforos_cfg[i]));
		dictionary_put(semaforos, semaforos_cfg[i], atoi(kernel_conf->sem_init[i]));
		i++;
	}
}

/**
 * si el pid se encuentra en la cola de bloqueados, devuelve TRUE
 * pero ademas tira la magia de actualizarlo (si... es cualquiera, pero me ahorra tener que recorrer la cola dos veces)
 */
bool proceso_bloqueado(t_PCB* pcb){
	bool encontroPCB = false;
	bool _is_pcb(t_PCB* p) {
		if(p->pid == pcb->pid){
			encontroPCB = true;
		}
		return (p->pid == pcb->pid);
	}
	sem_wait(semColaBloqueados);
	// esto es un asco, pero bueno... elimino el viejo pcb de la cola de bloqueados y pongo el nuevo
	t_PCB* aux = list_remove_by_condition(cola_bloqueados, (void*) _is_pcb);
	if(encontroPCB){
		list_add(cola_bloqueados, pcb);
	}
	sem_post(semColaBloqueados);
	if(encontroPCB){
		pcb_destroy(aux);
	}

	return encontroPCB;
}

t_PCB* sacar_pcb(t_list* list, t_PCB* pcb){
	bool _is_pcb(t_PCB* p) {
		return (p->pid == pcb->pid);
	}
	t_PCB* pcbEncontrado = list_remove_by_condition(cola_bloqueados, (void*) _is_pcb);
	return pcbEncontrado;
}
