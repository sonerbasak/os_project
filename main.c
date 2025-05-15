#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#define TOTAL_CARS 12
#define TOTAL_MINIBUSES 10
#define TOTAL_TRUCKS 8
#define TOTAL_VEHICLES (TOTAL_CARS + TOTAL_MINIBUSES + TOTAL_TRUCKS)
#define CAPACITY 20

typedef enum { CAR = 1, MINIBUS = 2, TRUCK = 3 } VehicleType;
typedef struct {
    int id;
    VehicleType type;
    int start_side;
    int current_side;
    int returned;
} Vehicle;

Vehicle vehicles[TOTAL_VEHICLES];

sem_t *toll[4];
sem_t *square[2];

pthread_mutex_t ferry_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ferry_full = PTHREAD_COND_INITIALIZER;

pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
int start_signal_given = 0;

int ferry_load = 0;
int ferry_side;
int vehicles_on_ferry[CAPACITY];
int vehicle_count = 0;

int vehicles_waiting[2] = {0, 0};       // bekleme alanında
int pending_on_side[2] = {0, 0};        // gişeyi geçmeden önce
int vehicles_remaining = TOTAL_VEHICLES * 2;

const char* vehicle_type_str(VehicleType type) {
    switch (type) {
        case CAR: return "Otomobil";
        case MINIBUS: return "Minibüs";
        case TRUCK: return "Kamyon";
        default: return "Bilinmeyen";
    }
}

void *vehicle_thread(void *arg) {
    Vehicle *v = (Vehicle *)arg;

    pthread_mutex_lock(&start_mutex);
    while (!start_signal_given)
        pthread_cond_wait(&start_cond, &start_mutex);
    pthread_mutex_unlock(&start_mutex);

    for (int trip = 0; trip < 2; trip++) {
        pthread_mutex_lock(&ferry_mutex);
        pending_on_side[v->current_side]++;
        pthread_mutex_unlock(&ferry_mutex);

        int local_gate = rand() % 2;
        int toll_index = v->current_side * 2 + local_gate;

        printf("[Araç %d - %s] Taraf %d üzerindeki %d numaralı gişeyi bekliyor...\n",
               v->id, vehicle_type_str(v->type), v->current_side, local_gate);

        sem_wait(toll[toll_index]);
        printf("[Araç %d - %s] Taraf %d üzerindeki gişeden geçiyor...\n",
               v->id, vehicle_type_str(v->type), v->current_side);
        sleep(3);
        sem_post(toll[toll_index]);

        printf("[Araç %d - %s] Taraf %d bekleme alanında bekliyor...\n",
               v->id, vehicle_type_str(v->type), v->current_side);
        sem_wait(square[v->current_side]);
        sleep(3);

        pthread_mutex_lock(&ferry_mutex);
        pending_on_side[v->current_side]--;
        vehicles_waiting[v->current_side]++;
        pthread_mutex_unlock(&ferry_mutex);

        int boarded = 0;
        while (!boarded) {
            pthread_mutex_lock(&ferry_mutex);
            if (ferry_side == v->current_side &&
                ferry_load + v->type <= CAPACITY) {

                printf("[Araç %d - %s] Taraf %d üzerindeki feribota biniyor (dolu: %d/%d)...\n",
                       v->id, vehicle_type_str(v->type), v->current_side, ferry_load, CAPACITY);

                ferry_load += v->type;
                vehicles_on_ferry[vehicle_count++] = v->id;

                vehicles_waiting[v->current_side]--;
                vehicles_remaining--;

                pthread_cond_signal(&ferry_full);
                boarded = 1;
            }
            pthread_mutex_unlock(&ferry_mutex);
            if (!boarded) sleep(2);
        }

        while (v->current_side == ferry_side)
            sleep(2);

        printf("[Araç %d - %s] Feribottan indi. Yeni taraf: %d\n",
               v->id, vehicle_type_str(v->type), ferry_side);
        sem_post(square[ferry_side]);

        v->current_side = ferry_side;
        if (trip == 1) v->returned = 1;

        sleep(rand() % 5 + 3);
    }

    pthread_exit(NULL);
}

void *ferry_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&ferry_mutex);

        while (!start_signal_given)
            pthread_cond_wait(&start_cond, &start_mutex);
        pthread_mutex_unlock(&start_mutex);

        while (ferry_load < CAPACITY &&
               (vehicles_waiting[ferry_side] + pending_on_side[ferry_side]) > 0) {
            pthread_cond_wait(&ferry_full, &ferry_mutex);
        }

        if (vehicles_remaining == 0 && ferry_load == 0) {
            pthread_mutex_unlock(&ferry_mutex);
            break;
        }

        printf("\n=== Feribot taraf %d' den hareket ediyor (yük: %d/%d) ===\n", ferry_side, ferry_load, CAPACITY);
        sleep(4);
        ferry_side = 1 - ferry_side;
        printf("=== Feribot taraf %d'ye ulaştı ===\n\n", ferry_side);

        ferry_load = 0;
        vehicle_count = 0;

        pthread_mutex_unlock(&ferry_mutex);
        sleep(3);
    }

    pthread_exit(NULL);
}

void init_named_semaphores() {
    for (int i = 0; i < 4; ++i) {
        char name[16];
        sprintf(name, "/toll%d", i);
        sem_unlink(name);
        toll[i] = sem_open(name, O_CREAT, 0644, 1);
    }

    for (int i = 0; i < 2; ++i) {
        char name[16];
        sprintf(name, "/square%d", i);
        sem_unlink(name);
        square[i] = sem_open(name, O_CREAT, 0644, CAPACITY);
    }
}

void cleanup_named_semaphores() {
    for (int i = 0; i < 4; ++i) {
        char name[16];
        sprintf(name, "/toll%d", i);
        sem_unlink(name);
        sem_close(toll[i]);
    }

    for (int i = 0; i < 2; ++i) {
        char name[16];
        sprintf(name, "/square%d", i);
        sem_unlink(name);
        sem_close(square[i]);
    }
}

int main() {
    srand(time(NULL));
    pthread_t vthreads[TOTAL_VEHICLES];
    pthread_t fthread;

    init_named_semaphores();

    ferry_side = rand() % 2;
    printf("Feribot başlangıç tarafı: %d\n", ferry_side);

    int id = 0;
    for (int i = 0; i < TOTAL_CARS; ++i, ++id)
        vehicles[id] = (Vehicle){id, CAR, rand() % 2, rand() % 2, 0};
    for (int i = 0; i < TOTAL_MINIBUSES; ++i, ++id)
        vehicles[id] = (Vehicle){id, MINIBUS, rand() % 2, rand() % 2, 0};
    for (int i = 0; i < TOTAL_TRUCKS; ++i, ++id)
        vehicles[id] = (Vehicle){id, TRUCK, rand() % 2, rand() % 2, 0};
    
    for (int i = 0; i < TOTAL_VEHICLES; ++i)
        pthread_create(&vthreads[i], NULL, vehicle_thread, &vehicles[i]);


    pthread_mutex_lock(&start_mutex);
    start_signal_given = 1;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_mutex);

    pthread_create(&fthread, NULL, ferry_thread, NULL);

    for (int i = 0; i < TOTAL_VEHICLES; ++i)
        pthread_join(vthreads[i], NULL);

    pthread_join(fthread, NULL);

    cleanup_named_semaphores();

    printf("\nTüm araçlar başlangıç tarafına geri döndü. Program sona erdi.\n");
    return 0;
}
