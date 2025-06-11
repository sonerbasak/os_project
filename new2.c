#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h> // For clock_gettime
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
    struct timespec start_time;    // Time vehicle entered the system
    struct timespec end_time;      // Time vehicle exited the system
    long long total_wait_time;     // Total waiting time for the vehicle (nanoseconds)
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

int vehicles_waiting[2] = {0, 0};       // Vehicles in waiting area
int pending_on_side[2] = {0, 0};        // Vehicles before passing the toll gate
int vehicles_remaining = TOTAL_VEHICLES * 2; // Initially, each vehicle makes 2 trips (round trip)

// System-wide time measurements
struct timespec simulation_start_time;
struct timespec simulation_end_time;

const char* vehicle_type_str(VehicleType type) {
    switch (type) {
        case CAR: return "Car";
        case MINIBUS: return "Minibus";
        case TRUCK: return "Truck";
        default: return "Unknown";
    }
}

void *vehicle_thread(void *arg) {
    Vehicle *v = (Vehicle *)arg;
    struct timespec wait_start, wait_end; // For general waiting time measurement

    // Record the time the vehicle enters the system
    clock_gettime(CLOCK_MONOTONIC, &v->start_time);

    pthread_mutex_lock(&start_mutex);
    while (!start_signal_given) {
        pthread_cond_wait(&start_cond, &start_mutex);
    }
    pthread_mutex_unlock(&start_mutex);

    for (int trip = 0; trip < 2; trip++) {
        pthread_mutex_lock(&ferry_mutex);
        pending_on_side[v->current_side]++;
        pthread_mutex_unlock(&ferry_mutex);

        int local_gate = rand() % 2;
        int toll_index = v->current_side * 2 + local_gate;

        printf("[Vehicle %d - %s] Waiting for gate %d on Side %d...\n",
               v->id, vehicle_type_str(v->type), local_gate, v->current_side);
        
        // Gate waiting start
        clock_gettime(CLOCK_MONOTONIC, &wait_start);
        sem_wait(toll[toll_index]);
        clock_gettime(CLOCK_MONOTONIC, &wait_end);
        v->total_wait_time += (wait_end.tv_sec - wait_start.tv_sec) * 1000000000LL +
                              (wait_end.tv_nsec - wait_start.tv_nsec);

        printf("[Vehicle %d - %s] Passing through gate on Side %d...\n",
               v->id, vehicle_type_str(v->type), v->current_side);
        sleep(3);
        sem_post(toll[toll_index]);

        printf("[Vehicle %d - %s] Waiting in holding area on Side %d...\n",
               v->id, vehicle_type_str(v->type), v->current_side);

        // Holding area waiting start
        clock_gettime(CLOCK_MONOTONIC, &wait_start);
        sem_wait(square[v->current_side]);
        clock_gettime(CLOCK_MONOTONIC, &wait_end);
        v->total_wait_time += (wait_end.tv_sec - wait_start.tv_sec) * 1000000000LL +
                              (wait_end.tv_nsec - wait_start.tv_nsec);
        sleep(3);

        pthread_mutex_lock(&ferry_mutex);
        pending_on_side[v->current_side]--;
        vehicles_waiting[v->current_side]++;
        pthread_mutex_unlock(&ferry_mutex);

        int boarded = 0;
        // Ferry waiting start
        clock_gettime(CLOCK_MONOTONIC, &wait_start);
        while (!boarded) {
            pthread_mutex_lock(&ferry_mutex);
            if (ferry_side == v->current_side &&
                ferry_load + v->type <= CAPACITY) {

                printf("[Vehicle %d - %s] Boarding ferry on Side %d (load: %d/%d)...\n",
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
        // Ferry waiting end
        clock_gettime(CLOCK_MONOTONIC, &wait_end);
        v->total_wait_time += (wait_end.tv_sec - wait_start.tv_sec) * 1000000000LL +
                              (wait_end.tv_nsec - wait_start.tv_nsec);
        
        while (v->current_side == ferry_side) {
            sleep(2);
        }

        printf("[Vehicle %d - %s] Disembarked from ferry. New side: %d\n",
               v->id, vehicle_type_str(v->type), ferry_side);
        sem_post(square[ferry_side]);

        v->current_side = ferry_side;
        if (trip == 1) { // Round trip completed
            v->returned = 1;
            // Record the time the vehicle exits the system
            clock_gettime(CLOCK_MONOTONIC, &v->end_time);
        }

        sleep(rand() % 5 + 3);
    }

    pthread_exit(NULL);
}

void *ferry_thread(void *arg) {
    // Record the simulation start time (when ferry thread starts)
    clock_gettime(CLOCK_MONOTONIC, &simulation_start_time);

    while (1) {
        pthread_mutex_lock(&start_mutex);
        while (!start_signal_given) {
            pthread_cond_wait(&start_cond, &start_mutex);
        }
        pthread_mutex_unlock(&start_mutex);

        pthread_mutex_lock(&ferry_mutex);
        while (ferry_load < CAPACITY &&
               (vehicles_waiting[ferry_side] + pending_on_side[ferry_side]) > 0) {
            // If all vehicles have returned and the ferry is empty, terminate the thread
            if (vehicles_remaining == 0 && ferry_load == 0 && 
                vehicles_waiting[0] == 0 && vehicles_waiting[1] == 0 && 
                pending_on_side[0] == 0 && pending_on_side[1] == 0) {
                pthread_mutex_unlock(&ferry_mutex);
                goto end_ferry_thread;
            }
            pthread_cond_wait(&ferry_full, &ferry_mutex);
        }

        // Check again if simulation should end after waiting
        if (vehicles_remaining == 0 && ferry_load == 0 && 
            vehicles_waiting[0] == 0 && vehicles_waiting[1] == 0 && 
            pending_on_side[0] == 0 && pending_on_side[1] == 0) {
            pthread_mutex_unlock(&ferry_mutex);
            break;
        }

        printf("\n=== Ferry departing from Side %d (load: %d/%d) ===\n", ferry_side, ferry_load, CAPACITY);
        sleep(4);
        ferry_side = 1 - ferry_side;
        printf("=== Ferry arrived at Side %d ===\n\n", ferry_side);
        
        ferry_load = 0;
        vehicle_count = 0;

        pthread_mutex_unlock(&ferry_mutex);
        sleep(3);
    }

end_ferry_thread:
    // Record the simulation end time (when ferry thread terminates)
    clock_gettime(CLOCK_MONOTONIC, &simulation_end_time);
    pthread_exit(NULL);
}

void init_named_semaphores() {
    for (int i = 0; i < 4; ++i) {
        char name[16];
        sprintf(name, "/toll%d", i);
        sem_unlink(name); // Clean up any previous semaphores
        toll[i] = sem_open(name, O_CREAT, 0644, 1);
        if (toll[i] == SEM_FAILED) {
            perror("sem_open toll failed");
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < 2; ++i) {
        char name[16];
        sprintf(name, "/square%d", i);
        sem_unlink(name); // Clean up any previous semaphores
        square[i] = sem_open(name, O_CREAT, 0644, CAPACITY);
        if (square[i] == SEM_FAILED) {
            perror("sem_open square failed");
            exit(EXIT_FAILURE);
        }
    }
}

void cleanup_named_semaphores() {
    for (int i = 0; i < 4; ++i) {
        char name[16];
        sprintf(name, "/toll%d", i);
        sem_close(toll[i]);
        sem_unlink(name);
    }

    for (int i = 0; i < 2; ++i) {
        char name[16];
        sprintf(name, "/square%d", i);
        sem_close(square[i]);
        sem_unlink(name);
    }
}

int main() {
    srand(time(NULL));
    pthread_t vthreads[TOTAL_VEHICLES];
    pthread_t fthread;

    init_named_semaphores();

    ferry_side = rand() % 2;
    printf("Ferry starting side: %d\n\n", ferry_side);

    int id = 0;
    for (int i = 0; i < TOTAL_CARS; ++i, ++id) {
        vehicles[id] = (Vehicle){id, CAR, rand() % 2, rand() % 2, 0, {0,0}, {0,0}, 0LL};
    }
    for (int i = 0; i < TOTAL_MINIBUSES; ++i, ++id) {
        vehicles[id] = (Vehicle){id, MINIBUS, rand() % 2, rand() % 2, 0, {0,0}, {0,0}, 0LL};
    }
    for (int i = 0; i < TOTAL_TRUCKS; ++i, ++id) {
        vehicles[id] = (Vehicle){id, TRUCK, rand() % 2, rand() % 2, 0, {0,0}, {0,0}, 0LL};
    }
    
    // Start the ferry thread (to record simulation start time)
    pthread_create(&fthread, NULL, ferry_thread, NULL);

    // Start vehicle threads
    for (int i = 0; i < TOTAL_VEHICLES; ++i) {
        pthread_create(&vthreads[i], NULL, vehicle_thread, &vehicles[i]);
    }

    // Signal all threads to start
    pthread_mutex_lock(&start_mutex);
    start_signal_given = 1;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_mutex);

    for (int i = 0; i < TOTAL_VEHICLES; ++i) {
        pthread_join(vthreads[i], NULL);
    }

    pthread_join(fthread, NULL);

    cleanup_named_semaphores();

    printf("\n--- Simulation Results ---\n");

    // Individual vehicle system times
    long long total_system_time_sum = 0;
    for (int i = 0; i < TOTAL_VEHICLES; ++i) {
        long long vehicle_duration_ns = (vehicles[i].end_time.tv_sec - vehicles[i].start_time.tv_sec) * 1000000000LL +
                                        (vehicles[i].end_time.tv_nsec - vehicles[i].start_time.tv_nsec);
        total_system_time_sum += vehicle_duration_ns;
        printf("Vehicle %d (%s) total time in system: %.4f seconds\n",
               vehicles[i].id, vehicle_type_str(vehicles[i].type), (double)vehicle_duration_ns / 1000000000.0);
    }

    // Individual vehicle waiting times
    long long total_wait_time_sum = 0;
    for (int i = 0; i < TOTAL_VEHICLES; ++i) {
        total_wait_time_sum += vehicles[i].total_wait_time;
        printf("Vehicle %d (%s) total waiting time: %.4f seconds\n", vehicles[i].id, 
               vehicle_type_str(vehicles[i].type), (double)vehicles[i].total_wait_time / 1000000000.0);
    }
    printf("----------------------------------\n");

    // Total simulation runtime
    long long total_sim_duration_ns = (simulation_end_time.tv_sec - simulation_start_time.tv_sec) * 1000000000LL +
                                      (simulation_end_time.tv_nsec - simulation_start_time.tv_nsec);
    printf("Total simulation runtime: %.4f seconds\n", (double)total_sim_duration_ns / 1000000000.0);
    // Average time vehicles spent in the system
    if (TOTAL_VEHICLES > 0) {
        double average_system_time = (double)total_system_time_sum / TOTAL_VEHICLES / 1000000000.0;
        printf("Average time vehicles spent in system: %.4f seconds\n", average_system_time);
    }
    // Average waiting time for all vehicles
    if (TOTAL_VEHICLES > 0) {
        double average_total_wait_time = (double)total_wait_time_sum / TOTAL_VEHICLES / 1000000000.0;
        printf("Average waiting time for all vehicles: %.4f seconds\n", average_total_wait_time);
    }
    printf("----------------------------------\n");

    printf("\nAll vehicles have returned to their starting side. Program ended.\n");
    return 0;
}