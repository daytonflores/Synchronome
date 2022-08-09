#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

//#define MY_CLOCK CLOCK_REALTIME
//#define MY_CLOCK CLOCK_MONOTONIC
#define MY_CLOCK CLOCK_MONOTONIC_RAW
//#define MY_CLOCK CLOCK_REALTIME_COARSE
//#define MY_CLOCK CLOCK_MONOTONIC_COARSE

#define NUM_OF_THREADS  2

typedef struct
{
    int threadIdx;
} threadParams_t;

const int SUCCESS           = 0;            ///< Success return code
const int ERROR             = -1;           ///< Error return code
const int NSEC_PER_SEC      = 1000000000;   ///< Amount of ns per sec
const int THREAD_IDX_UPDATE = 1;            ///< Thread ID that will update location
const int THREAD_IDX_READ   = 2;            ///< Thread ID that will read location
const int RUN_TIME          = 180;          ///< Run program for 180 sec
const int PERIOD_UPDATE     = 1;            ///< Update location every 1 sec
const int PERIOD_READ       = 10;           ///< Read location every 10 sec

pthread_t threads[NUM_OF_THREADS];                              ///< POSIX thread declarations
pthread_mutex_t mutex_location = PTHREAD_MUTEX_INITIALIZER;     ///< Mutex declaration for location memory access

struct location{
    struct timespec time;   ///< The current time t in {sec, ns}
    double latitude;        ///< f(t) = 0.01t
    double longitude;       ///< f(t) = 0.2t
    double altitude;        ///< f(t) = 0.25t
    double roll;            ///< f(t) = sin(t)
    double pitch;           ///< f(t) = cos(t^2)
    double yaw;             ///< f(t) = cos(t)
};

struct location location_now = {{0, 0}, 0, 0, 0, 0, 0, 0};  ///< Unsafe global to update and read from with different threads

static struct timespec time_passed_update = {0, 0}; ///< Time taken by update thread to complete
static struct timespec time_passed_read = {0, 0};   ///< Time taken by read thread to complete

///< \fn int delta_t(struct timespec* stop, struct timespec* start, struct timespec* delta_t)
///< \brief Calculates different between two timespecs
///< \param struct timespec* stop - the later time
///< \param struct timespec* start - the earlier time
///< \param struct timespec* delta_t - the difference
///< \return int SUCCESS or ERROR
int delta_t(struct timespec* stop, struct timespec* start, struct timespec* delta_t)
{
    int dt_sec = stop->tv_sec - start->tv_sec;
    int dt_nsec = stop->tv_nsec - start->tv_nsec;

    //printf("\ndt calcuation\n");

    // case 1 - less than a second of change
    if (dt_sec == 0)
    {
        //printf("dt less than 1 second\n");

        if (dt_nsec >= 0 && dt_nsec < NSEC_PER_SEC)
        {
            //printf("nanosec greater at stop than start\n");
            delta_t->tv_sec = 0;
            delta_t->tv_nsec = dt_nsec;
        }

        else if (dt_nsec > NSEC_PER_SEC)
        {
            //printf("nanosec overflow\n");
            delta_t->tv_sec = 1;
            delta_t->tv_nsec = dt_nsec - NSEC_PER_SEC;
        }

        else // dt_nsec < 0 means stop is earlier than start
        {
            //printf("stop is earlier than start\n");
            return(ERROR);
        }
    }

    // case 2 - more than a second of change, check for roll-over
    else if (dt_sec > 0)
    {
        //printf("dt more than 1 second\n");

        if (dt_nsec >= 0 && dt_nsec < NSEC_PER_SEC)
        {
            //printf("nanosec greater at stop than start\n");
            delta_t->tv_sec = dt_sec;
            delta_t->tv_nsec = dt_nsec;
        }

        else if (dt_nsec > NSEC_PER_SEC)
        {
            //printf("nanosec overflow\n");
            delta_t->tv_sec = delta_t->tv_sec + 1;
            delta_t->tv_nsec = dt_nsec - NSEC_PER_SEC;
        }

        else // dt_nsec < 0 means roll over
        {
            //printf("nanosec roll over\n");
            delta_t->tv_sec = dt_sec - 1;
            delta_t->tv_nsec = NSEC_PER_SEC + dt_nsec;
        }
    }

    return(SUCCESS);
}

void *update_location(void *threadp)
{
    int return_code;                        ///< Holds return code for function calls
    int i;                                  ///< Iteration counter
    struct timespec time_start = {0, 0};    ///< Time taken as loop begins
    struct timespec time_end = {0, 0};      ///< Time taken as loop exits
    struct timespec time_passed = {0, 0};   ///< Time taken per iteration i
    struct timespec time_now = {0, 0};      ///< Current time to measure against PERIOD_UPDATE

    ///< Initialize iteration counter
    i = 0;

    ///< Record start time before entering loop
    clock_gettime(MY_CLOCK, &time_start);
    //printf("Starting update at %ld\n", time_start.tv_sec);

    ///< Stay in this while loop for RUN_TIME seconds
    while (i <= RUN_TIME) {
        //printf("UPDATE - Start = %ld, i = %d, passed = %ld\n", time_start.tv_sec, i, time_passed.tv_sec);

        ///< Stay in this do-while loop for PERIOD_UPDATE seconds
        do {
            clock_gettime(MY_CLOCK, &time_now);
            delta_t(&time_now, &time_start, &time_passed);
            //printf("UPDATE - Start = %ld, i = %d, passed = %ld\n", time_start.tv_sec, i, time_passed.tv_sec);
        } while (time_passed.tv_sec < (i + PERIOD_UPDATE));

        ///< Once PERIOD_UPDATE seconds have passed, lock the mutex and begin updating location_now
        ///< Then unlock the mutex and increase iteration counter
        pthread_mutex_lock(&mutex_location);
        location_now.time = time_passed;
        location_now.latitude = 0.01 * (((double)location_now.time.tv_sec + ((double)location_now.time.tv_nsec / NSEC_PER_SEC)));    ///< f(t) = 0.01t
        location_now.longitude = 0.2 * (((double)location_now.time.tv_sec + ((double)location_now.time.tv_nsec / NSEC_PER_SEC)));    ///< f(t) = 0.2t
        location_now.altitude = 0.25 * (((double)location_now.time.tv_sec + ((double)location_now.time.tv_nsec / NSEC_PER_SEC)));    ///< f(t) = 0.25t
        location_now.roll = sin(((double)location_now.time.tv_sec + ((double)location_now.time.tv_nsec / NSEC_PER_SEC)));            ///< f(t) = sin(t)
        location_now.pitch = cos(pow(((double)location_now.time.tv_sec + ((double)location_now.time.tv_nsec / NSEC_PER_SEC)), 2));   ///< f(t) = cos(t^2)
        location_now.yaw = cos(((double)location_now.time.tv_sec + ((double)location_now.time.tv_nsec / NSEC_PER_SEC)));             ///< f(t) = cos(t)
        pthread_mutex_unlock(&mutex_location);
        i = i + PERIOD_UPDATE;
    }

    ///< Record end time after exiting loop
    clock_gettime(MY_CLOCK, &time_end);

    ///< Find time taken for update loop to finish
    delta_t(&time_end, &time_start, &time_passed_update);
    //printf("UPDATE - Start = %ld, i = %d, passed = %ld, end = %ld, passed = %ld\n", time_start.tv_sec, i, time_passed.tv_sec, time_end.tv_sec, time_passed_update.tv_sec);
}


void *read_location(void *threadp)
{
    int return_code;                        ///< Holds return code for function calls
    int i;                                  ///< Iteration counter
    struct timespec time_start = {0, 0};    ///< Time taken as loop begins
    struct timespec time_end = {0, 0};      ///< Time taken as loop exits
    struct timespec time_passed = {0, 0};   ///< Time taken per iteration i
    struct timespec time_now = {0, 0};      ///< Current time to measure against PERIOD_UPDATE

    ///< Initialize iteration counter
    i = 0;

    ///< Record start time before entering loop
    clock_gettime(MY_CLOCK, &time_start);
    //printf("Starting read at %ld\n", time_start.tv_sec);

    ///< Stay in this while loop for RUN_TIME seconds
    while (i <= RUN_TIME) {
        //printf("READ - Start = %ld, i = %d, passed = %ld\n", time_start.tv_sec, i, time_passed.tv_sec);

        ///< Stay in this do-while loop for PERIOD_READ seconds
        do {
            clock_gettime(MY_CLOCK, &time_now);
            delta_t(&time_now, &time_start, &time_passed);
            //printf("READ - Start = %ld, i = %d, passed = %ld\n", time_start.tv_sec, i, time_passed.tv_sec);
        } while ((time_passed.tv_sec) < (i + PERIOD_READ));

        ///< Once PERIOD_RATE seconds have passed, lock the mutex and begin reading location_now
        ///< Then unlock the mutex and increase iteration counter
        pthread_mutex_lock(&mutex_location);
        printf("time = %ld sec,\tlatitude = %f,\tlongitude = %f,\taltitude = %f,\troll = %f,\tpitch = %f,\tyaw = s%f\n", (location_now.time.tv_sec + (location_now.time.tv_nsec/NSEC_PER_SEC)), location_now.latitude, location_now.longitude, location_now.altitude, location_now.roll, location_now.pitch, location_now.yaw);
        pthread_mutex_unlock(&mutex_location);
        i = i + PERIOD_READ;
    }

    ///< Record end time after exiting loop
    clock_gettime(MY_CLOCK, &time_end);

    ///< Find time taken for update loop to finish
    delta_t(&time_end, &time_start, &time_passed_read);
    //printf("READ - Start = %ld, i = %d, loop = %ld, passed = %ld, end = %ld, passed = %ld\n", time_start.tv_sec, i, time_loop.tv_sec, time_passed.tv_sec, time_end.tv_sec, time_passed_read.tv_sec);
}

int main (int argc, char *argv[])
{
    int return_code;                        ///< Holds return code for function calls
    int i;                                  ///< Iteration counter
    struct timespec time_start = {0, 0};    ///< Time taken as threads are created
    struct timespec time_end = {0, 0};      ///< Time taken as threads are joined
    struct timespec time_passed = {0, 0};   ///< Time taken for threads to execute

    ///< Set MY_CLOCK resolution
    return_code = clock_getres(MY_CLOCK, &(location_now.time));

    if (return_code != SUCCESS){
        perror("clock_getres");
        exit(ERROR);
    }
    else{
       printf("\nPOSIX Clock demo using system RT clock with resolution:\n\t%ld secs, %ld microsecs, %ld nanosecs\n\n", location_now.time.tv_sec, (location_now.time.tv_nsec / 1000), location_now.time.tv_nsec);
    }

    printf("Updating location every %d sec\n", PERIOD_UPDATE);
    printf("Reading location every %d sec\n", PERIOD_READ);
    printf("Over a total period of %d sec\n\n", RUN_TIME);

    ///< Record start time before creating threads
    clock_gettime(MY_CLOCK, &time_start);

    ///< Create threads with default attributes
    ///< and enter respective functions
    for (i = 1; i <= NUM_OF_THREADS; i = i + 1) {
        if (i == THREAD_IDX_UPDATE) {
            return_code = pthread_create(&threads[i - 1],    ///< pointer to 0-indexed thread descriptor
                                        (void*)0,           ///< use default thread attributes
                                        update_location,    ///< thread function entry point
                                        (void*)0            ///< no parameters to pass to function
           );

           if (return_code != SUCCESS) {
               printf("Failed to create update thread %d\n", i);
               exit(ERROR);
           }
       }
       else if (i == THREAD_IDX_READ) {
           return_code = pthread_create(&threads[i - 1],    ///< pointer to 0-indexed thread descriptor
                                        (void*)0,           ///< use default thread attributes
                                        read_location,      ///< thread function entry point
                                        (void*)0            ///< no parameters to pass to function
           );

            if (return_code != SUCCESS) {
                printf("Failed to create read thread %d\n", i);
                exit(ERROR);
            }
       }
    }

    ///< Waits for thread[i] to terminate and then joins it back to the main thread
    for (i = 1; i < NUM_OF_THREADS; i = i + 1)
    {
       pthread_join(threads[i - 1], NULL);
    }

    ///< Record end time after joining threads
    clock_gettime(MY_CLOCK, &time_end);

    ///< Find time taken for threads to complete
    delta_t(&time_end, &time_start, &time_passed);

    printf("\n");
    printf("Executed for %ld sec\n", (time_passed.tv_sec + time_passed.tv_nsec / NSEC_PER_SEC));
    //printf("Updated data for %ld sec\n", (time_passed_update.tv_sec + time_passed_update.tv_nsec / NSEC_PER_SEC));
    //printf("Read data for %ld sec\n", (time_passed_read.tv_sec + time_passed_read.tv_nsec / NSEC_PER_SEC));
    printf("\n");
    printf("TEST COMPLETE\n\n");

    return(SUCCESS);
}
