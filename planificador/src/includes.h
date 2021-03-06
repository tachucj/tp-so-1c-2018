#ifndef SRC_INCLUDES_H_
#define SRC_INCLUDES_H_

#include <stdio.h>
#include <stdlib.h>

#include "commons/collections/list.h"
#include "commons/collections/dictionary.h"
#include "commons/string.h"
#include "semaphore.h"

typedef struct{
	int PUERTO;
	char* ALGORITMO_PLANIFICACION;
	int ALFA_PLANIFICACION;
	int ESTIMACION_INICIAL;
	char* IP_COORDINADOR;
	int PUERTO_COORDINADOR;
	char** CLAVES_BLOQUEADAS;
}configuracion_t;

typedef struct{
	int idProceso;
	int socketESI;
	t_list* clavesBloqueadas;
	char* claveBloqueo;
	double rafagaEstimada;
	double rafagaActual;
	double rafagasEsperando;
	int exitStatus;
	char* nombreESI;
}proceso_t;

typedef struct {
	int socket_escucha_esis;
	int socket_coordinador;
} sockets_escucha_t;

typedef enum id_exit_status {
	exit_ok = 1,
	exit_abortado_por_clave_inexistente = 2,
	exit_abortado_por_consola = 3,
	exit_abortado_inesperado = 4
} id_exit_status;

typedef struct {
	int respuestaACoordinador;
	int fdESIAAbortar;
} respuesta_operacion_t;

typedef struct {
	int tamanioValor;
	char* valor;
	int tamanioNombreInstanciaClave;
	char* nombreInstanciaClave;
	int tamanioNombreInstanciaCandidata;
	char* nombreInstanciaCandidata;
	int tamanioClave;
	char* clave;
} status_clave_t;

#endif /* SRC_INCLUDES_H_ */
