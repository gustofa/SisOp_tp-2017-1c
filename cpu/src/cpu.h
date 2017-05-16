/*
 * cpu.h
 *
 *  Created on: 15/5/2017
 *      Author: utnso
 */

#ifndef CPU_H_
#define CPU_H_

typedef struct{
	char* memory_ip;
	uint32_t memory_port;
	char* kernel_ip;
	uint32_t kernel_port;
}t_cpu_conf;

void load_properties(void);
void inicializarFuncionesParser(void);
#endif /* CPU_H_ */
