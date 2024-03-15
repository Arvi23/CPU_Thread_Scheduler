#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include "so_scheduler.h"

#define MAX_THREADS 1024

struct thread
{
	pthread_t thread;		// Thread ID
	int priority;			// The priority of the thread
	int time_quantum;		// The time quantum
	sem_t semaphore;		// A semaphore
	int event_id;			// The event ID
	so_handler *so_handler; // The handler funciton
};

struct thread_scheduler
{
	pthread_t all_threads[MAX_THREADS]; // The IDs of all threads
	int nr_all_threads;
	pthread_mutex_t lock;				   // A lock for the scheduler
	struct thread *ready_thr[MAX_THREADS]; // The queue of threads in ready state
	int nr_ready;
	struct thread *current_thread;		  // The currently-executing thread
	int time_quantum;					  // The time quantum for each thread
	int num_io_devices;					  // The number of IO devices supported
	struct thread *wait_thr[MAX_THREADS]; // The array of waiting threads
	int nr_waiting;
};

int nr_init; // The number of times the scheduler has been initialized
// The thread scheduler
static struct thread_scheduler *scheduler = NULL;

// queue functions
// Adds the thread to the queue depending on the priority
void add_queue(struct thread *thread)
{
	if (scheduler->nr_ready == 0)
	{
		scheduler->ready_thr[scheduler->nr_ready++] = thread;
		return;
	}

	for (int i = 0; i < scheduler->nr_ready; i++)
	{
		if (thread->priority <= scheduler->ready_thr[i]->priority)
		{
			for (int j = scheduler->nr_ready; j > i; j--)
			{
				scheduler->ready_thr[j] = scheduler->ready_thr[j - 1];
			}

			thread->time_quantum = scheduler->time_quantum;
			scheduler->ready_thr[i] = thread;
			scheduler->nr_ready++;
			return;
		}
	}
	scheduler->ready_thr[scheduler->nr_ready++] = thread;
}
// Returns the thread with the highest priority
struct thread *get_queue()
{
	if (scheduler->nr_ready)
	{
		struct thread *tmp = scheduler->ready_thr[--scheduler->nr_ready];
		scheduler->ready_thr[scheduler->nr_ready] = NULL;
		return tmp;
	}
	return NULL;
}

// Adds the thread to the waiting array

void add_waiting(struct thread *thread, unsigned int io)
{
	thread->event_id = io;

	scheduler->wait_thr[scheduler->nr_waiting++] = thread;
}
// Wakes up the thead(s) waiting for the given IO, adding them to the ready queue
int wakeup_waiting(unsigned int io)
{
	unsigned int nr = 0;
	for (int i = 0; i < scheduler->nr_waiting; i++)
	{
		if (scheduler->wait_thr[i]->event_id == io)
		{
			struct thread *woken_thread = scheduler->wait_thr[i];

			for (int j = i; j < scheduler->nr_waiting - 1; j++)
				scheduler->wait_thr[j] = scheduler->wait_thr[j + 1];

			add_queue(woken_thread);

			nr++;
			i--;
			scheduler->nr_waiting--;
		}
	}

	return nr;
}
// Either puts a thread in current_thread or signals there are no more threads to be scheduled
void switch_thread()
{
	//If there is no current_thread we get one from the queue
	if (scheduler->current_thread == NULL)
	{
		scheduler->current_thread = get_queue();
		//If that one is NULL as well then we unlock the scheduler and return
		if (scheduler->current_thread == NULL)
		{
			pthread_mutex_unlock(&scheduler->lock);
			return;
		}
		sem_post(&scheduler->current_thread->semaphore);
		return;
	}
	//If the time has passed or there is a higher priority than the current thread
	//We add the current thread in the queue and switch to the new thread
	//The ex-thread's semaphore is switched to waiting, the new one is switched to post
	if (scheduler->current_thread->time_quantum <= 0 || (scheduler->nr_ready > 0 && scheduler->current_thread->priority < scheduler->ready_thr[scheduler->nr_ready - 1]->priority))
	{
		struct thread *last_current_thread = scheduler->current_thread;
		add_queue(scheduler->current_thread);
		scheduler->current_thread = get_queue();
		sem_post(&scheduler->current_thread->semaphore);
		sem_wait(&last_current_thread->semaphore);
		return;
	}
}
//The start_thread routine for each thread
//It calls the handler function
void *start_thread(void *arg)
{
	struct thread *my_thread = (struct thread *)arg;

	sem_wait(&my_thread->semaphore);

	my_thread->so_handler(my_thread->priority);

	scheduler->all_threads[scheduler->nr_all_threads++] = my_thread->thread;
	sem_destroy(&my_thread->semaphore);
	free(my_thread);
	//When the curent thread finished then we change the current thread
	scheduler->current_thread = NULL;
	switch_thread();
}

int so_init(unsigned int time_quantum, unsigned int io)
{
	if (io > SO_MAX_NUM_EVENTS)
		return -1;
	// Check if time_quantum and io are valid values
	if (time_quantum <= 0 || io < 0)
	{
		// Return an error code or print an error message
		return -1;
	}
	//Checks if the scheduler has been already initialized
	if (nr_init > 0)
	{
		return -1;
	}
	// Create and initialize the thread scheduler
	scheduler = malloc(sizeof(struct thread_scheduler));
	pthread_mutex_init(&scheduler->lock, NULL);
	pthread_mutex_lock(&scheduler->lock);

	scheduler->nr_ready = 0;
	scheduler->nr_waiting = 0;
	scheduler->nr_all_threads = 0;
	scheduler->current_thread = NULL;
	scheduler->time_quantum = time_quantum;
	scheduler->num_io_devices = io;
	if (scheduler == NULL)
	{
		return -1;
	}
	nr_init = nr_init + 1;
	return 0;
}
// Starts a new thread
tid_t so_fork(so_handler *func, unsigned int priority)
{
	// Check if the scheduler exists and if the paramaters given are valid
	if (func == NULL)
	{
		return INVALID_TID;
	}
	if (priority > SO_MAX_PRIO)
	{
		return INVALID_TID;
	}
	if (scheduler == NULL)
	{
		return INVALID_TID;
	}
	// Creates the thread
	struct thread *new_thread = malloc(sizeof(struct thread));
	new_thread->priority = priority;
	new_thread->time_quantum = scheduler->time_quantum;
	new_thread->so_handler = func;

	sem_init(&new_thread->semaphore, 0, 0);

	pthread_t thread;

	pthread_create(&new_thread->thread, NULL, start_thread, new_thread);

	thread = new_thread->thread;

	add_queue(new_thread);

	if (scheduler->current_thread != NULL)
		scheduler->current_thread->time_quantum--;
	//Update the current thread if needed
	switch_thread();
	return thread;
}

int so_wait(unsigned int io)
{
	//Checks if the IO is valid
	if (io >= scheduler->num_io_devices)
	{

		return -1;
	}

	struct thread *last_current_thread = scheduler->current_thread;
	//Adding the current thread to the waiting array
	add_waiting(scheduler->current_thread, io);

	scheduler->current_thread = NULL;

	switch_thread();
	//Blocking it's execution
	sem_wait(&last_current_thread->semaphore);

	return 0;
}

int so_signal(unsigned int io)
{
	//Checks if the IO is valid
	if (io >= scheduler->num_io_devices)
		return -1;
	//Waking the waiting threads
	int woken_threads = wakeup_waiting(io);

	scheduler->current_thread->time_quantum--;

	switch_thread();

	return woken_threads;
}

void so_exec(void)
{
	scheduler->current_thread->time_quantum--;
	switch_thread();
}

void so_end(void)
{
	if (scheduler == NULL)
	{
		// The scheduler has already been destroyed or was never initialized
		return;
	}

	// Lock the scheduler
	if (scheduler->current_thread != NULL)
		pthread_mutex_lock(&scheduler->lock);
	pthread_mutex_unlock(&scheduler->lock);
	pthread_mutex_destroy(&scheduler->lock);
	//Joining all the threads that existed
	for (int i = 0; i < scheduler->nr_all_threads; i++)
		pthread_join(scheduler->all_threads[i], NULL);
	// Clear the scheduler
	free(scheduler);
	nr_init--;

	return;
}