#include "planificacion.h"
#include "includes.h"
#include "conexiones.h"
#include "comunicacion/comunicacion.h"
#include <commons/log.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>

t_list* colaListos;
proceso_t* procesoEjecucion;
t_queue* colaTerminados;
t_list* listaBloqueados;
t_dictionary* claves;
t_log* logPlanificador;
pthread_mutex_t mutex_lista_bloqueados;

extern configuracion_t config;
int contadorProcesos = 1;

//Prototipos
bool esFIFO();
bool esSJFCD();
bool esSJFSD();
bool esHRRN();
bool planificadorConDesalojo();
bool comparadorSJF(void* arg1, void* arg2);
bool comparadorHRRN(void* arg1, void* arg2);
void ordenarColaListos();
void planificarConDesalojo();
void procesoBloquear(char* clave);
void incrementarRafagasEsperando();
bool solicitarClave(char* clave);
void liberarClave(char* clave);
void colaListosPush(proceso_t* proceso);
proceso_t* colaListosPop();
proceso_t* colaListosPeek();
int procesoEjecutar(proceso_t* proceso);
void estimarRafaga(proceso_t* proceso);
void liberarRecursos(proceso_t* proceso);
void desbloquearClave(char* clave);

//Funciones
bool esFIFO(){
	char *FIFO = "FIFO";
	return strcmp(config.ALGORITMO_PLANIFICACION, FIFO)==0;
}

bool esSJFCD(){
	char *SJFCD= "SJF-CD";
	return strcmp(config.ALGORITMO_PLANIFICACION, SJFCD)==0;
}

bool esSJFSD(){
	char *SJFSD= "SJF-SD";
	return strcmp(config.ALGORITMO_PLANIFICACION, SJFSD)==0;
}

bool esHRRN(){
	char *HRRN= "HRRN";
	return strcmp(config.ALGORITMO_PLANIFICACION, HRRN)==0;
}

void inicializarPlanificacion(){
	colaListos = list_create();
	procesoEjecucion = 0;
	colaTerminados = queue_create();
	listaBloqueados = list_create();
	claves = dictionary_create();
	char** p = config.CLAVES_BLOQUEADAS;
	while(p[0]){
		bloquearClave(p[0]);
		p++;
	}
	sem_init(&planificacion_habilitada, 1, 1);
}

bool planificadorConDesalojo(){
	return esSJFCD() || esHRRN();
}

bool comparadorSJF(void* arg1, void* arg2){
	proceso_t* proceso1 = (proceso_t*)arg1;
	proceso_t* proceso2 = (proceso_t*)arg2;
	return proceso1->rafagaEstimada <= proceso2->rafagaEstimada;
}

bool comparadorHRRN(void* arg1, void* arg2){
	proceso_t* proceso1 = (proceso_t*)arg1;
	proceso_t* proceso2 = (proceso_t*)arg2;
	return (1 + proceso1->rafagasEsperando/proceso1->rafagaEstimada) >= (1 + proceso2->rafagasEsperando/proceso2->rafagaEstimada);
}

void ordenarColaListos(){
	if(esHRRN()){
		list_sort(colaListos, &comparadorHRRN);
	}else if(esFIFO()){
		//No ordena
	}else{
		list_sort(colaListos, &comparadorSJF);
	}

}

void planificarConDesalojo(){
	if(list_size(colaListos)){
		bool cambiarProceso;
		if(esHRRN()){
			proceso_t* proceso1 = colaListosPeek();
			proceso_t* proceso2 = procesoEjecucion;
			cambiarProceso = (1 + proceso1->rafagasEsperando/proceso1->rafagaEstimada) > (1 + proceso2->rafagasEsperando/proceso2->rafagaEstimada);
		}else{
			proceso_t* proceso1 = colaListosPeek();
			proceso_t* proceso2 = procesoEjecucion;
			cambiarProceso = proceso1->rafagaEstimada < proceso2->rafagaEstimada;
		}
		if(cambiarProceso){
			colaListosPush(procesoEjecucion);
			procesoEjecutar(colaListosPop());
		}else{
			procesoEjecutar(procesoEjecucion);
		}
	}else{
		procesoEjecutar(procesoEjecucion);
	}
}

void colaListosPush(proceso_t* proceso){
	list_add(colaListos, (void*)proceso);
	ordenarColaListos();
}

proceso_t* colaListosPop(){
	return list_remove(colaListos, 0);
}

proceso_t* colaListosPeek(){
	return list_get(colaListos, 0);
}

int procesoNuevo(int socketESI) {
	proceso_t* proceso = malloc(sizeof(proceso_t));
	proceso->idProceso = contadorProcesos;
	contadorProcesos++;
	proceso->claveBloqueo = string_new();
	proceso->clavesBloqueadas = list_create();
	proceso->rafagaActual = 0;
	proceso->rafagaEstimada = config.ESTIMACION_INICIAL;
	proceso->rafagasEsperando = 0;
	proceso->socketESI = socketESI;
	int retorno;

	if(procesoEjecucion == 0){
		retorno = procesoEjecutar(proceso);
		if (retorno < 0) {
			procesoTerminado(exit_abortado_inesperado);
		}
	} else {
		colaListosPush(proceso);
		if (planificadorConDesalojo()) {
			planificarConDesalojo();
		}
	}

	return EXIT_SUCCESS; //TODO: Revisar si habría que hacer algún return antes en los IF

}

int procesoEjecutar(proceso_t* proceso) {
	procesoEjecucion = proceso;
	sem_wait(&planificacion_habilitada);

	int resultado =  mandar_a_ejecutar_esi(proceso->socketESI);
	sem_post(&planificacion_habilitada);
	return resultado;
}

void procesoTerminado(int exitStatus) {
	procesoEjecucion->exitStatus = exitStatus;
	queue_push(colaTerminados, (void*)procesoEjecucion);

	pthread_mutex_lock(&mutex_cola_listos); //PROBLEMA MUTEX.
	pthread_mutex_lock(&mutex_lista_bloqueados);
	liberarRecursos(procesoEjecucion);
	pthread_mutex_unlock(&mutex_lista_bloqueados);
	pthread_mutex_unlock(&mutex_cola_listos);


	proceso_t* procesoListo = colaListosPop();
	if (procesoListo != NULL) {
		procesoEjecutar(procesoListo);
	} else {
		procesoEjecucion = 0;
	}
}

void procesoBloquear(char* clave){
	procesoEjecucion->claveBloqueo=clave;
	list_add(listaBloqueados, (void*)procesoEjecucion);
	if(colaListosPeek()){
		procesoEjecutar(colaListosPop());
	}else{
		procesoEjecucion = 0;
	}
}

void procesoDesbloquear(char* clave) {

	//Closure para hacer el remove de la lista de procesos bloqueadas
	bool _soy_esi_bloqueado_por_clave_buscada(proceso_t* p) {
		return strcmp(p->claveBloqueo, clave) == 0;
	}

	proceso_t* proceso = (proceso_t*)list_remove_by_condition(listaBloqueados, (void*)_soy_esi_bloqueado_por_clave_buscada);

	if (proceso != NULL) { //Desbloqueo el proceso para esa clave
		estimarRafaga(proceso);
		if (procesoEjecucion == 0) {
			procesoEjecutar(proceso);
		} else {
			colaListosPush(proceso);
			if (planificadorConDesalojo()) {
				planificarConDesalojo();
			}
		}
		if (!list_any_satisfy(listaBloqueados, (void*)_soy_esi_bloqueado_por_clave_buscada)) { //Si no quedaron bloqueados liberamos la clave
			desbloquearClave(clave);
		}
	} else { //Si no había proceso para esa clave nos aseguramos de que no quede en el diccionario de bloqueadas
		desbloquearClave(clave);
	}

}

void incrementarRafagasEsperando() {
	int i;
	for (i=0;i<list_size(colaListos);i++) {
		proceso_t* proceso;
		proceso = (proceso_t*)list_get(colaListos, i);
		proceso->rafagasEsperando++;
	}
}

void sentenciaFinalizada(int socket_esi) {
	procesoEjecucion->rafagaActual++;
	if (esHRRN()) {
		incrementarRafagasEsperando();
	}
	//El socket que envió sentencia finalizada difiere del que está en ejecución => Fue desalojado (bloqueado) por consola.
	if (socket_esi == procesoEjecucion->socketESI) {
		if (planificadorConDesalojo()) {
			planificarConDesalojo();
		} else {
			procesoEjecutar(procesoEjecucion);
			 //TODO: Manejar error Send.
		}
	}
}

void estimarRafaga(proceso_t* proceso) {
	int alfa = config.ALFA_PLANIFICACION;
	int estimacion = proceso->rafagaActual*alfa/100 + (100-alfa)*proceso->rafagaEstimada/100;
	proceso->rafagaEstimada = estimacion;
}

bool verificarClave(char* clave){
	bool _soy_clave_buscada(char* p) {
		return strcmp(p, clave) == 0;
	}
	return !list_any_satisfy(procesoEjecucion->clavesBloqueadas, (void*)_soy_clave_buscada) && dictionary_has_key(claves, clave);
}

void bloquearClave(char* clave){
	dictionary_put(claves, clave, 0);
}

void desbloquearClave(char* clave) {
	dictionary_remove(claves, clave);
}

bool solicitarClave(char* clave){
	bool pudo_solicitar = true;
	if(verificarClave(clave)){
		procesoBloquear(clave);
		pudo_solicitar = false;
	} else {
		bloquearClave(clave);
		list_add(procesoEjecucion->clavesBloqueadas, clave);
	}
	return pudo_solicitar;
}

//Siempre se llamará luego de asegurarse que el proceso tenga la clave bloqueada
void liberarClave(char* clave){
	//Closure para hacer el remove de la lista de claves bloqueadas
	bool _soy_clave_buscada(char* p) {
		return strcmp(p, clave) == 0;
	}
	list_remove_by_condition(procesoEjecucion->clavesBloqueadas, (void*)_soy_clave_buscada);
	desbloquearClave(clave);
}

void liberarRecursos(proceso_t* proceso) {
	log_debug(logPlanificador, "Liberando claves tomadas por proceso %d...", proceso->idProceso);


	list_iterate(proceso->clavesBloqueadas, (void*)procesoDesbloquear);


	log_debug(logPlanificador, "Se liberaron las claves tomadas por el proceso %d", proceso->idProceso);
}

void bloquearEsiPorConsola(int idEsi, char* clave) {
	//Closure para hacer el remove de la lista de claves bloqueadas
	bool _soy_clave_buscada(proceso_t* p) {
		return p->idProceso == idEsi;
	}
	proceso_t* proceso = (proceso_t*)list_remove_by_condition(colaListos, (void*)_soy_clave_buscada);
	if (proceso != NULL) {
		//Bloqueo manual de proceso que estaba en Listo.
		proceso->claveBloqueo = clave;
		list_add(listaBloqueados, proceso);
		if(!verificarClave(clave)) //Si la clave no estaba bloqueada también hay que bloquear la clave
			bloquearClave(clave);
	} else {
		if (procesoEjecucion->idProceso == idEsi) {
			if(!verificarClave(clave))
				bloquearClave(clave);
			procesoBloquear(clave);
		}
	}

}

void listarRecursosBloqueadosPorClave(char* clave) {
	//Closure para recorrer la lista de proceso bloqueados por la clave y guardar el string a imprimir
	char* stringLog = string_new();

	void _listarProceso(proceso_t* p) {
		if (strcmp(p->claveBloqueo, clave) == 0) {
			char* idString = string_new();
			sprintf(idString, "%d", p->idProceso);
			string_append(&stringLog, idString);
			string_append(&stringLog, "-");
			free(idString);
		}
	}
	list_iterate(listaBloqueados, (void*)_listarProceso);
	char* stringLogFinal = (!string_is_empty(stringLog)) ? string_substring_until(stringLog, string_length(stringLog) - 1) : "";
	log_info(logPlanificador, "Procesos bloqueados: %s", stringLogFinal);

	free(stringLog);
}

t_list* killProcesoPorID(int idProceso) {
	//Closure para buscar proceso por id
	bool _soy_proceso_buscado(proceso_t* p) {
		return p->idProceso == idProceso;
	}

	proceso_t* procesoATerminar = NULL;

	sem_wait(&planificacion_habilitada);
	if (procesoEjecucion != NULL && procesoEjecucion->idProceso == idProceso) {
		procesoATerminar = malloc(sizeof(proceso_t));
		procesoATerminar->idProceso = procesoEjecucion->idProceso;
		procesoATerminar->clavesBloqueadas = list_create();
		list_add_all(procesoATerminar->clavesBloqueadas, procesoEjecucion->clavesBloqueadas);
		procesoTerminado(exit_abortado_por_consola);
	} else {
		procesoATerminar = list_remove_by_condition(listaBloqueados, (void*)_soy_proceso_buscado);
		//TODO Armar un procesoDestroy;
		if (procesoATerminar != NULL) {
			liberarRecursos(procesoATerminar);
			procesoATerminar->exitStatus = exit_abortado_por_consola;
		} else {
			proceso_t* procesoATerminar = list_remove_by_condition(colaListos, (void*)_soy_proceso_buscado);
			if (procesoATerminar != NULL) {
				liberarRecursos(procesoATerminar);
				procesoATerminar->exitStatus = exit_abortado_por_consola;
				queue_push(colaTerminados, (void*)procesoATerminar);
			}
		}
	}
	sem_post(&planificacion_habilitada);

	return (procesoATerminar != NULL) ? procesoATerminar->clavesBloqueadas : NULL;
}

void analizarDeadlocks() {
	t_list* listaDeadlock = list_create();

	/********** Armado del Grafo **********/
	t_list* listaTemporal = list_create();

	//Agregamos al análisis sólo los procesos que están bloqueando claves
	void _add_if_blocking(proceso_t* pr) {
		if (pr->clavesBloqueadas != NULL && !list_is_empty(pr->clavesBloqueadas)) {
			list_add(listaTemporal, pr);
		}
	}
	list_iterate(listaBloqueados, (void*)_add_if_blocking);

	struct Nodo {
		int pid;
		struct Nodo* bloqueador;
	};

	int size = list_size(listaTemporal);
	struct Nodo* nodos[size];
	int i;
	//Inicializamos array de Nodos
	for(i=0; i < size; i++) {
		struct Nodo* nodo = (struct Nodo*)malloc(sizeof(struct Nodo));
		proceso_t* proceso = list_get(listaTemporal, i);
		nodos[i] = nodo;
		nodo->pid = proceso->idProceso;
		nodo->bloqueador = NULL;
	}

	for(i=0; i < size; i++) {
		proceso_t* proceso = list_get(listaTemporal, i);

		bool _soy_proceso_bloqueador(proceso_t* p) {
			bool _soy_clave_buscada(char* clave) {
				return strcmp(clave, proceso->claveBloqueo) == 0;
			}
			return list_any_satisfy(p->clavesBloqueadas, (void*)_soy_clave_buscada);
		}
		proceso_t * procesoBloqueador = list_find(listaBloqueados, (void*)_soy_proceso_bloqueador);
		if (procesoBloqueador != NULL) {
			int j;
			for(j=0; j < size; j++) {
				if(nodos[j]->pid == procesoBloqueador->idProceso) {
					break;
				}
			}
			nodos[i]->bloqueador = nodos[j];
		}
	}
	/********** Fin del armado del Grafo **********/

	/********** Buscamos esperas circulares (deadlocks) *********/
	bool deadlock;
	for(i=0; i < size; i++) { //Arrancamos la búsqueda desde cada nodo
		deadlock = false;
		bool _ya_esta_en_deadlock(struct Nodo* n) {
			return n->pid == nodos[i]->pid;
		}
		//Si ya está en la lista de deadlock lo salteo para evitar duplicados
		if (list_any_satisfy(listaDeadlock, (void*)_ya_esta_en_deadlock))
			continue;

		struct Nodo* nodoInicial = nodos[i];
		struct Nodo* nodo = nodoInicial->bloqueador;
		bool fin = 0;
		int iteraciones = 0;
		t_list* candidatosDeadlock = list_create();
		list_add(candidatosDeadlock, nodoInicial);
		while(!fin && iteraciones < size) { //Seguimos los nodos bloqueadores hasta ver si cerramos el círculo y llegamos de nuevo al nodo inicial (espera circular)
			if (nodo == NULL) { //NO hay deadlock
				fin = true;
			} else if (nodo->pid == nodoInicial->pid) { //Cerramos el círculo. Hay Deadlock.
				deadlock = true;
				fin = true;
			} else { //Continuamos buscando
				list_add(candidatosDeadlock, nodo);
				nodo = nodo->bloqueador;
			}
			iteraciones++;
		}
		if (deadlock) {
			list_add_all(listaDeadlock, candidatosDeadlock);
		}

		//TODO Eliminar los nodos antes
		free(candidatosDeadlock);
	}
	/********** Fin Búsqueda de esperas circulares *********/

	//Closure para recorrer la lista de proceso bloqueados por la clave y guardar el string a imprimir
	char* stringLog = string_new();

	void _listarProcesosDeadlock(struct Nodo* n) {
		char* idString = string_new();
		sprintf(idString, "%d", n->pid);
		string_append(&stringLog, idString);
		string_append(&stringLog, "-");
		free(idString);
	}

	list_iterate(listaDeadlock, (void*)_listarProcesosDeadlock);

	char* stringLogFinal = (!string_is_empty(stringLog)) ? string_substring_until(stringLog, string_length(stringLog) - 1) : "";
	log_info(logPlanificador, "Procesos en deadlock: %s", stringLogFinal);

}

int fdProcesoEnEjecucion() {
	return procesoEjecucion->socketESI;
}

respuesta_operacion_t procesar_notificacion_coordinador(int comando, int tamanio, void* cuerpo) {

	respuesta_operacion_t retorno;

	//Closure para buscar si el proceso tiene la clave que intenta bloquear
	bool _soy_clave_buscada(char* p) {
		return strcmp(p, (char*)cuerpo) == 0;
	}

	//El cuerpo siempre tiene que ser una clave
	switch(comando) {
	case msj_solicitud_get_clave: //Procesar GET
		log_debug(logPlanificador, "Notificacion Coordinador - Solicitud de GET clave recibida: %s", cuerpo);
		retorno.respuestaACoordinador = solicitarClave(cuerpo);
		log_debug(logPlanificador, "Pudo solicitar clave? => %d", retorno.respuestaACoordinador);
		retorno.fdESIAAbortar = -1;
		break;
	case msj_store_clave: //Procesar STORE
		log_debug(logPlanificador, "Notificacion Coordinador - Solicitud de STORE clave recibida");
		liberarClave(cuerpo);
		procesoDesbloquear(cuerpo);
		retorno.respuestaACoordinador = 1;
		retorno.fdESIAAbortar = -1;
		break;
	case msj_error_clave_no_identificada: //Procesar inexistancia clave
		log_debug(logPlanificador, "Notificacion Coordinador - Error clave no identificada recibido");
		retorno.fdESIAAbortar = procesoEjecucion->socketESI;
		procesoTerminado(exit_abortado_por_clave_inexistente);
		retorno.respuestaACoordinador = 1;
		break;
	case msj_esi_tiene_tomada_clave:
		log_debug(logPlanificador, "Notificacion Coordinador - Consulta ESI por clave tomada recibida");
		retorno.respuestaACoordinador = list_any_satisfy(procesoEjecucion->clavesBloqueadas, (void*)_soy_clave_buscada);
		log_debug(logPlanificador, "Esi tiene tomada clave %s? => %d", (char*)cuerpo, retorno.respuestaACoordinador);
		retorno.fdESIAAbortar = (retorno.respuestaACoordinador == 1) ? -1 : procesoEjecucion->socketESI;
		break;
	default:
		log_debug(logPlanificador, "Notificacion Coordinador - Llegó un mensaje desconocido: %d", comando);
		break;
	}


	return retorno;

}
