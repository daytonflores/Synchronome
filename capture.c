/*
 *
 *  Adapted by Sam Siewert for use with UVC web cameras and Bt878 frame
 *  grabber NTSC cameras to acquire digital video from a source,
 *  time-stamp each frame acquired, save to a PGM or PPM file.
 *
 *  The original code adapted was open source from V4L2 API and had the
 *  following use and incorporation policy:
 * 
 *  This program can be used and distributed without restrictions.
 *
 *      This program is provided with the V4L2 API
 * see http://linuxtv.org/docs.php for more information
 */

 ///< This is necessary for CPU affinity macros in Linux
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>             /* getopt_long() */

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include <time.h>

#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <syslog.h>
#include <sys/sysinfo.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))
//#define COLOR_CONVERT_RGB
#define HRES 640
#define VRES 480
#define HRES_STR "640"
#define VRES_STR "480"
//#define HRES 320
//#define VRES 240
//#define HRES_STR "320"
//#define VRES_STR "240"

/*************************************************************************
* Insert my code below
*************************************************************************/

///< Which clock to utilize in Linux
#define MY_CLOCK CLOCK_REALTIME
//#define MY_CLOCK CLOCK_MONOTONIC
//#define MY_CLOCK CLOCK_MONOTONIC_RAW
//#define MY_CLOCK CLOCK_REALTIME_COARSE
//#define MY_CLOCK CLOCK_MONOTONIC_COARSE

///< Return codes
#define TRUE (1)
#define FALSE (0)
#define EXIT_SUCCESS (0)
#define EXIT_FAILURE (1)
#define EXIT_FAILURE2 (-1)

///< Conversion values
#define NANOSEC_PER_SEC (1000000000)
#define NANOSEC_PER_MICROSEC (1000)
#define MILLISEC_PER_SEC (1000)
#define SEC_PER_MIN (60)

///< Increase this if you fall behind over time
#define CLOCK_BIAS_MICROSEC (100)
#define CLOCK_BIAS_NANOSEC (100000)

///< Number of RT services requiring their own threads
///< S0 - Sequencer for controlling clock
///< S1 - Frame Acquisition for capturing frames of clock
///< S2 - Frame Difference Threshold for matrix subtraction to see if seconds hand has moved
///< S3 - Frame Select for selecting the "best" frame to send to Frame Write-Back buffer
///< S4 - Frame Process for processing the frame (in my case, to grayscale)
///< S5 - Frame Write-Back for writing the processed frame from buffer to FLASH
#define NUM_OF_THREADS 6

///< Define if we want to take into account absolute time in S0 Sequencer
#define ABS_DELAY

///< Define if we want to take into account drift control in S0 Sequencer
//#define DRIFT_CONTROL

///< (DT_SCALING_UNCERTAINTY_MS)*(10^9 ns/1 s)/(1 s/1000 ms)
///< Example (1) - If 0.5 ms is desired for sleep dt scaling uncertainty, then (DT_SCALING_UNCERTAINTY_MS)*(10^9 ns/1 s)/(1000 ms/1 s) = 500000
///<               Thus, for 0.5 ms sleep dt scaling uncertainty this would be set to 500000
#define DT_SCALING_UNCERTAINTY_MILLISEC (0.5)
#define DT_SCALING_UNCERTAINTY_NANOSEC (DT_SCALING_UNCERTAINTY_MILLISEC*(NANOSEC_PER_SEC/MILLISEC_PER_SEC))

///< Desired frequencies in Hz for all S0-S5
#define S0_FREQ (120)
#define S1_FREQ (20)
#define S2_FREQ (2)
#define S3_FREQ (1)
#define S4_FREQ (0.5)
#define S5_FREQ (0.25)

///< Desired time to run in s
#define S0_RUN_TIME_SEC (60)
#define S0_RUN_TIME_MIN (S0_RUN_TIME_SEC/SEC_PER_MIN)

///< How many periods S0 Sequencer should run for
#define S0_PERIODS (S0_FREQ*S0_RUN_TIME_SEC)

///< (10^9 ns)/(1 s * S0_FREQ)
///< Example (1) - If 100 Hz is desired for the Sequencer, then (10^9 ns)/(1 s * 100 Hz) = (10^9)/(100 Hz) = 10000000
///<               Thus, for 100 Hz this would be set to 10000000
///< Example (2) - If 120 Hz is desired for the Sequencer, then (10^9 ns)/(1 s * 120 Hz) = (10^9)/(120 Hz) = 8333333
///<               Thus, for 120 Hz this would be set to 8333333
#define RTSEQ_DELAY_NSEC (NANOSEC_PER_SEC/S0_FREQ)

///< Size of buffer to hold tail syslog trace command at the end
///< Example (1) - tail -216000 /var/log/syslog | grep -n FinalProject > ./syslog_trace_30min.txt
///<               The above command is 78 characters + 1 character for NULL
#define SYS_BUF_SIZE (78 + 1)

///< Raspberry Pi 4b+ has 4 cores
#define NUM_OF_CPU_CORES 4

///< All services S0 will run on this core
#define RT_CORE 2

///< Structure to store thread IDs + number of S0 Sequencer periods
typedef struct
{
    int threadIdx;
    unsigned long long sequencePeriods;
} threadParams_t;

///< POSIX thread declarations
pthread_t threads[NUM_OF_THREADS];

///< POSIX thread parameters to pass per entry point
threadParams_t threadParams[NUM_OF_THREADS];

///< POSIX scheduling parameters to create scheduling attributes with for main thread + service threads
struct sched_param main_param;
struct sched_param rt_param[NUM_OF_THREADS];

///< POSIX scheduling attributes for main thread + service threads
pthread_attr_t main_attr;
pthread_attr_t rt_sched_attr[NUM_OF_THREADS];

///< Semaphore for each service S1-S5 all controlled by S0 Sequencer
sem_t semS1;
sem_t semS2;
sem_t semS3;
sem_t semS4;
sem_t semS5;

///< Keep track of global times in ms
static double start_time = 0;
static double end_time = 0;

///< Controls for aborting all services S0-S5
int abortS0 = FALSE;
int abortS1 = FALSE;
int abortS2 = FALSE;
int abortS3 = FALSE;
int abortS4 = FALSE;
int abortS5 = FALSE;

/*************************************************************************
* Insert my code above
*************************************************************************/

// Format is used by a number of functions, so made as a file global
static struct v4l2_format fmt;

enum io_method 
{
        IO_METHOD_READ,
        IO_METHOD_MMAP,
        IO_METHOD_USERPTR,
};

struct buffer 
{
        void   *start;
        size_t  length;
};

static char            *dev_name;
//static enum io_method   io = IO_METHOD_USERPTR;
//static enum io_method   io = IO_METHOD_READ;
static enum io_method   io = IO_METHOD_MMAP;
static int              fd = -1;
struct buffer          *buffers;
static unsigned int     n_buffers;
static int              out_buf;
static int              force_format=1;
static int              frame_count = (189);

static void errno_exit(const char* s)
{
#ifdef PRINTF_ALL
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
#elif SYSLOG_ALL
    syslog(LOG_ERROR, "%s error %d, %s\n", s, errno, strerror(errno));
#endif

    exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void* arg)
{
    int r;

    do
    {
        r = ioctl(fh, request, arg);

    } while (-1 == r && EINTR == errno);

    return r;
}

char ppm_header[] = "P6\n#9999999999 sec 9999999999 msec \n"HRES_STR" "VRES_STR"\n255\n";
char ppm_dumpname[] = "frames/test0000.ppm";

static void dump_ppm(const void* p, int size, unsigned int tag, struct timespec* time)
{
    int written, i, total, dumpfd;

    snprintf(&ppm_dumpname[11], 9, "%04d", tag);
    strncat(&ppm_dumpname[15], ".ppm", 5);
    dumpfd = open(ppm_dumpname, O_WRONLY | O_NONBLOCK | O_CREAT, 00666);

    snprintf(&ppm_header[4], 11, "%010d", (int)time->tv_sec);
    strncat(&ppm_header[14], " sec ", 5);
    snprintf(&ppm_header[19], 11, "%010d", (int)((time->tv_nsec) / 1000000));
    strncat(&ppm_header[29], " msec \n"HRES_STR" "VRES_STR"\n255\n", 19);
    written = write(dumpfd, ppm_header, sizeof(ppm_header));

    total = 0;

    do
    {
        written = write(dumpfd, p, size);
        total += written;
    } while (total < size);

#ifdef PRINTF_ALL
    printf("wrote %d bytes\n", total);
#elif SYSLOG_ALL
    syslog(LOG_INFO, "wrote %d bytes\n", total);
#endif

    close(dumpfd);

}


char pgm_header[] = "P5\n#9999999999 sec 9999999999 msec \n"HRES_STR" "VRES_STR"\n255\n";
char pgm_dumpname[] = "frames/test0000.pgm";

static void dump_pgm(const void* p, int size, unsigned int tag, struct timespec* time)
{
    int written, i, total, dumpfd;

    snprintf(&pgm_dumpname[11], 9, "%04d", tag);
    strncat(&pgm_dumpname[15], ".pgm", 5);
    dumpfd = open(pgm_dumpname, O_WRONLY | O_NONBLOCK | O_CREAT, 00666);

    snprintf(&pgm_header[4], 11, "%010d", (int)time->tv_sec);
    strncat(&pgm_header[14], " sec ", 5);
    snprintf(&pgm_header[19], 11, "%010d", (int)((time->tv_nsec) / 1000000));
    strncat(&pgm_header[29], " msec \n"HRES_STR" "VRES_STR"\n255\n", 19);
    written = write(dumpfd, pgm_header, sizeof(pgm_header));

    total = 0;

    do
    {
        written = write(dumpfd, p, size);
        total += written;
    } while (total < size);

#ifdef PRINTF_ALL
    printf("wrote %d bytes\n", total);
#elif SYSLOG_ALL
    syslog(LOG_INFO, "wrote %d bytes\n", total);
#endif

    close(dumpfd);

}


void yuv2rgb_float(float y, float u, float v,
    unsigned char* r, unsigned char* g, unsigned char* b)
{
    float r_temp, g_temp, b_temp;

    // R = 1.164(Y-16) + 1.1596(V-128)
    r_temp = 1.164 * (y - 16.0) + 1.1596 * (v - 128.0);
    *r = r_temp > 255.0 ? 255 : (r_temp < 0.0 ? 0 : (unsigned char)r_temp);

    // G = 1.164(Y-16) - 0.813*(V-128) - 0.391*(U-128)
    g_temp = 1.164 * (y - 16.0) - 0.813 * (v - 128.0) - 0.391 * (u - 128.0);
    *g = g_temp > 255.0 ? 255 : (g_temp < 0.0 ? 0 : (unsigned char)g_temp);

    // B = 1.164*(Y-16) + 2.018*(U-128)
    b_temp = 1.164 * (y - 16.0) + 2.018 * (u - 128.0);
    *b = b_temp > 255.0 ? 255 : (b_temp < 0.0 ? 0 : (unsigned char)b_temp);
}


// This is probably the most acceptable conversion from camera YUYV to RGB
//
// Wikipedia has a good discussion on the details of various conversions and cites good references:
// http://en.wikipedia.org/wiki/YUV
//
// Also http://www.fourcc.org/yuv.php
//
// What's not clear without knowing more about the camera in question is how often U & V are sampled compared
// to Y.
//
// E.g. YUV444, which is equivalent to RGB, where both require 3 bytes for each pixel
//      YUV422, which we assume here, where there are 2 bytes for each pixel, with two Y samples for one U & V,
//              or as the name implies, 4Y and 2 UV pairs
//      YUV420, where for every 4 Ys, there is a single UV pair, 1.5 bytes for each pixel or 36 bytes for 24 pixels

void yuv2rgb(int y, int u, int v, unsigned char* r, unsigned char* g, unsigned char* b)
{
    int r1, g1, b1;

    // replaces floating point coefficients
    int c = y - 16, d = u - 128, e = v - 128;

    // Conversion that avoids floating point
    r1 = (298 * c + 409 * e + 128) >> 8;
    g1 = (298 * c - 100 * d - 208 * e + 128) >> 8;
    b1 = (298 * c + 516 * d + 128) >> 8;

    // Computed values may need clipping.
    if (r1 > 255) r1 = 255;
    if (g1 > 255) g1 = 255;
    if (b1 > 255) b1 = 255;

    if (r1 < 0) r1 = 0;
    if (g1 < 0) g1 = 0;
    if (b1 < 0) b1 = 0;

    *r = r1;
    *g = g1;
    *b = b1;
}


// always ignore first 8 frames
int framecnt = -8;

unsigned char bigbuffer[(1280 * 960)];

static void process_image(const void* p, int size)
{
    int i, newi, newsize = 0;
    struct timespec frame_time;
    int y_temp, y2_temp, u_temp, v_temp;
    unsigned char* pptr = (unsigned char*)p;

    // record when process was called
    clock_gettime(CLOCK_REALTIME, &frame_time);

    framecnt++;

#ifdef PRINTF_ALL
    printf("frame %d: ", framecnt);
#elif SYSLOG_ALL
    syslog(LOG_INFO, "frame %d: ", framecnt);
#endif

    // This just dumps the frame to a file now, but you could replace with whatever image
    // processing you wish.
    //

    if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_GREY)
    {
#ifdef PRINTF_ALL
        printf("Dump graymap as-is size %d\n", size);
#elif SYSLOG_ALL
        syslog(LOG_INFO, "Dump graymap as-is size %d\n", size);
#endif

        dump_pgm(p, size, framecnt, &frame_time);
    }

    else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
    {

#if defined(COLOR_CONVERT_RGB)

        // Pixels are YU and YV alternating, so YUYV which is 4 bytes
        // We want RGB, so RGBRGB which is 6 bytes
        //
        for (i = 0, newi = 0; i < size; i = i + 4, newi = newi + 6)
        {
            y_temp = (int)pptr[i]; u_temp = (int)pptr[i + 1]; y2_temp = (int)pptr[i + 2]; v_temp = (int)pptr[i + 3];
            yuv2rgb(y_temp, u_temp, v_temp, &bigbuffer[newi], &bigbuffer[newi + 1], &bigbuffer[newi + 2]);
            yuv2rgb(y2_temp, u_temp, v_temp, &bigbuffer[newi + 3], &bigbuffer[newi + 4], &bigbuffer[newi + 5]);
        }

        if (framecnt > -1)
        {
            dump_ppm(bigbuffer, ((size * 6) / 4), framecnt, &frame_time);

#ifdef PRINTF_ALL
            printf("Dump YUYV converted to RGB size %d\n", size);
#elif SYSLOG_ALL
            syslog(LOG_INFO, "Dump YUYV converted to RGB size %d\n", size);
#endif

}
#else

        // Pixels are YU and YV alternating, so YUYV which is 4 bytes
        // We want Y, so YY which is 2 bytes
        //
        for (i = 0, newi = 0; i < size; i = i + 4, newi = newi + 2)
        {
            // Y1=first byte and Y2=third byte
            bigbuffer[newi] = pptr[i];
            bigbuffer[newi + 1] = pptr[i + 2];
        }

        if (framecnt > -1)
        {
            dump_pgm(bigbuffer, (size / 2), framecnt, &frame_time);

#ifdef PRINTF_ALL
            printf("Dump YUYV converted to YY size %d\n", size);
#elif SYSLOG_ALL
            syslog(LOG_INFO, "Dump YUYV converted to YY size %d\n", size);
#endif
        }
#endif

    }

    else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB24)
    {
#ifdef PRINTF_ALL
        printf("Dump RGB as-is size %d\n", size);
#elif SYSLOG_ALL
        syslog(LOG_INFO, "Dump RGB as-is size %d\n", size);
#endif

        dump_ppm(p, size, framecnt, &frame_time);
    }
    else
    {
#ifdef PRINTF_ALL
        printf("ERROR - unknown dump format\n");
#elif SYSLOG_ALL
        syslog(LOG_ERR, "ERROR - unknown dump format\n");
#endif

    }

    fflush(stderr);

#ifdef PRINTF_ALL
    //fprintf(stderr, ".");
#elif SYSLOG_ALL
    //syslog(LOG_ERR, ".");
#endif

    fflush(stdout);
}

static int read_frame(void)
{
    struct v4l2_buffer buf;
    unsigned int i;

    switch (io)
    {

    case IO_METHOD_READ:
        if (-1 == read(fd, buffers[0].start, buffers[0].length))
        {
            switch (errno)
            {

            case EAGAIN:
                return 0;

            case EIO:
                /* Could ignore EIO, see spec. */

                /* fall through */

            default:
                errno_exit("read");
            }
        }

        process_image(buffers[0].start, buffers[0].length);
        break;

    case IO_METHOD_MMAP:
        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf))
        {
            switch (errno)
            {
            case EAGAIN:
                return 0;

            case EIO:
                /* Could ignore EIO, but drivers should only set for serious errors, although some set for
                   non-fatal errors too.
                 */
                return 0;


            default:
#ifdef PRINTF_ALL
                //printf("mmap failure\n");
#elif SYSLOG_ALL
                //syslog(LOG_INFO, "mmap failure\n");
#endif

                errno_exit("VIDIOC_DQBUF");
            }
        }

        assert(buf.index < n_buffers);

        process_image(buffers[buf.index].start, buf.bytesused);

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF");
        break;

    case IO_METHOD_USERPTR:
        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;

        if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf))
        {
            switch (errno)
            {
            case EAGAIN:
                return 0;

            case EIO:
                /* Could ignore EIO, see spec. */

                /* fall through */

            default:
                errno_exit("VIDIOC_DQBUF");
            }
        }

        for (i = 0; i < n_buffers; ++i)
            if (buf.m.userptr == (unsigned long)buffers[i].start
                && buf.length == buffers[i].length)
                break;

        assert(i < n_buffers);

        process_image((void*)buf.m.userptr, buf.bytesused);

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF");
        break;
    }

#ifdef PRINTF_ALL
    //printf("R");
#elif SYSLOG_ALL
    //syslog(LOG_INFO, "R");
#endif

    return 1;
}

static void mainloop(void)
{
    unsigned int count;
    struct timespec read_delay;
    struct timespec time_error;

    // Replace this with a sequencer DELAY
    //
    // 250 million nsec is a 250 msec delay, for 4 fps
    // 1 sec for 1 fps
    //
    read_delay.tv_sec = 1;
    read_delay.tv_nsec = 0;

    count = frame_count;

    while (count > 0)
    {
        for (;;)
        {
            fd_set fds;
            struct timeval tv;
            int r;

            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            /* Timeout. */
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            r = select(fd + 1, &fds, NULL, NULL, &tv);

            if (-1 == r)
            {
                if (EINTR == errno)
                    continue;
                errno_exit("select");
            }

            if (0 == r)
            {
#ifdef PRINTF_ALL
                fprintf(stderr, "select timeout\n");
#elif SYSLOG_ALL
                syslog(LOG_ERR, "select timeout\n");
#endif

                exit(EXIT_FAILURE);
            }

            if (read_frame())
            {
                if (nanosleep(&read_delay, &time_error) != 0) {
#ifdef PRINTF_ALL
                    perror("nanosleep");
#elif SYSLOG_ALL
                    syslog(LOG_ERR, "nanosleep");
#endif
                }


                else {
#ifdef PRINTF_ALL
                    printf("time_error.tv_sec=%ld, time_error.tv_nsec=%ld\n", time_error.tv_sec, time_error.tv_nsec);
#elif SYSLOG_ALL
                    syslog(LOG_INFO, "time_error.tv_sec=%ld, time_error.tv_nsec=%ld\n", time_error.tv_sec, time_error.tv_nsec);
#endif
                }

                count--;
                break;
            }

            /* EAGAIN - continue select loop unless count done. */
            if (count <= 0) break;
        }

        if (count <= 0) break;
    }
}

static void stop_capturing(void)
{
        enum v4l2_buf_type type;

        switch (io) {
        case IO_METHOD_READ:
                /* Nothing to do. */
                break;

        case IO_METHOD_MMAP:
        case IO_METHOD_USERPTR:
                type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
                        errno_exit("VIDIOC_STREAMOFF");
                break;
        }
}

static void start_capturing(void)
{
        unsigned int i;
        enum v4l2_buf_type type;

        switch (io) 
        {

        case IO_METHOD_READ:
                /* Nothing to do. */
                break;

        case IO_METHOD_MMAP:
                for (i = 0; i < n_buffers; ++i) 
                {
                        printf("allocated buffer %d\n", i);

                        struct v4l2_buffer buf;

                        CLEAR(buf);
                        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                        buf.memory = V4L2_MEMORY_MMAP;
                        buf.index = i;

                        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                                errno_exit("VIDIOC_QBUF");
                }
                type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
                        errno_exit("VIDIOC_STREAMON");
                break;

        case IO_METHOD_USERPTR:
                for (i = 0; i < n_buffers; ++i) {
                        struct v4l2_buffer buf;

                        CLEAR(buf);
                        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                        buf.memory = V4L2_MEMORY_USERPTR;
                        buf.index = i;
                        buf.m.userptr = (unsigned long)buffers[i].start;
                        buf.length = buffers[i].length;

                        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                                errno_exit("VIDIOC_QBUF");
                }
                type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
                        errno_exit("VIDIOC_STREAMON");
                break;
        }
}

static void uninit_device(void)
{
        unsigned int i;

        switch (io) {
        case IO_METHOD_READ:
                free(buffers[0].start);
                break;

        case IO_METHOD_MMAP:
                for (i = 0; i < n_buffers; ++i)
                        if (-1 == munmap(buffers[i].start, buffers[i].length))
                                errno_exit("munmap");
                break;

        case IO_METHOD_USERPTR:
                for (i = 0; i < n_buffers; ++i)
                        free(buffers[i].start);
                break;
        }

        free(buffers);
}

static void init_read(unsigned int buffer_size)
{
        buffers = calloc(1, sizeof(*buffers));

        if (!buffers) 
        {
                fprintf(stderr, "Out of memory\n");

                exit(EXIT_FAILURE);
        }

        buffers[0].length = buffer_size;
        buffers[0].start = malloc(buffer_size);

        if (!buffers[0].start) 
        {
                fprintf(stderr, "Out of memory\n");

                exit(EXIT_FAILURE);
        }
}

static void init_mmap(void)
{
        struct v4l2_requestbuffers req;

        CLEAR(req);

        req.count = 6;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) 
        {
                if (EINVAL == errno) 
                {
                        fprintf(stderr, "%s does not support "
                            "memory mapping\n", dev_name);

                        exit(EXIT_FAILURE);
                } else 
                {
                        errno_exit("VIDIOC_REQBUFS");
                }
        }

        if (req.count < 2) 
        {
                fprintf(stderr, "Insufficient buffer memory on %s\n", dev_name);

                exit(EXIT_FAILURE);
        }

        buffers = calloc(req.count, sizeof(*buffers));

        if (!buffers) 
        {
                fprintf(stderr, "Out of memory\n");

                exit(EXIT_FAILURE);
        }

        for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
                struct v4l2_buffer buf;

                CLEAR(buf);

                buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory      = V4L2_MEMORY_MMAP;
                buf.index       = n_buffers;

                if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
                        errno_exit("VIDIOC_QUERYBUF");

                buffers[n_buffers].length = buf.length;
                buffers[n_buffers].start =
                        mmap(NULL /* start anywhere */,
                              buf.length,
                              PROT_READ | PROT_WRITE /* required */,
                              MAP_SHARED /* recommended */,
                              fd, buf.m.offset);

                if (MAP_FAILED == buffers[n_buffers].start)
                        errno_exit("mmap");
        }
}

static void init_userp(unsigned int buffer_size)
{
        struct v4l2_requestbuffers req;

        CLEAR(req);

        req.count  = 4;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_USERPTR;

        if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
                if (EINVAL == errno) {
                        fprintf(stderr, "%s does not support "
                            "user pointer i/o\n", dev_name);

                        exit(EXIT_FAILURE);
                } else {
                        errno_exit("VIDIOC_REQBUFS");
                }
        }

        buffers = calloc(4, sizeof(*buffers));

        if (!buffers) {
                fprintf(stderr, "Out of memory\n");

                exit(EXIT_FAILURE);
        }

        for (n_buffers = 0; n_buffers < 4; ++n_buffers) {
                buffers[n_buffers].length = buffer_size;
                buffers[n_buffers].start = malloc(buffer_size);

                if (!buffers[n_buffers].start) {
                        fprintf(stderr, "Out of memory\n");
                        
                        exit(EXIT_FAILURE);
                }
        }
}

static void init_device(void)
{
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    unsigned int min;

    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap))
    {
        if (EINVAL == errno) {
            fprintf(stderr, "%s is no V4L2 device\n",
                dev_name);

            exit(EXIT_FAILURE);
        }
        else
        {
                errno_exit("VIDIOC_QUERYCAP");
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        fprintf(stderr, "%s is no video capture device\n",
            dev_name);

        exit(EXIT_FAILURE);
    }

    switch (io)
    {
        case IO_METHOD_READ:
            if (!(cap.capabilities & V4L2_CAP_READWRITE))
            {
                fprintf(stderr, "%s does not support read i/o\n",
                    dev_name);

                exit(EXIT_FAILURE);
            }
            break;

        case IO_METHOD_MMAP:
        case IO_METHOD_USERPTR:
            if (!(cap.capabilities & V4L2_CAP_STREAMING))
            {
                fprintf(stderr, "%s does not support streaming i/o\n",
                    dev_name);

                exit(EXIT_FAILURE);
            }
            break;
    }


    /* Select video input, video standard and tune here. */


    CLEAR(cropcap);

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap))
    {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */

        if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop))
        {
            switch (errno)
            {
                case EINVAL:
                    /* Cropping not supported. */
                    break;
                default:
                    /* Errors ignored. */
                        break;
            }
        }

    }
    else
    {
        /* Errors ignored. */
    }


    CLEAR(fmt);

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (force_format)
    {
        syslog(LOG_INFO, "FinalProject (MAIN): FORCING FORMAT\n");
        
        fmt.fmt.pix.width       = HRES;
        fmt.fmt.pix.height      = VRES;

        // Specify the Pixel Coding Formate here

        // This one works for Logitech C200
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

        //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
        //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_VYUY;

        // Would be nice if camera supported
        //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
        //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;

        //fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;

        if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
                errno_exit("VIDIOC_S_FMT");

        /* Note VIDIOC_S_FMT may change width and height. */
    }
    else
    {
        syslog(LOG_INFO, "FinalProject (MAIN): ASSUMING FORMAT\n");
        
        /* Preserve original settings as set by v4l2-ctl for example */
        if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
                    errno_exit("VIDIOC_G_FMT");
    }

    /* Buggy driver paranoia. */
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
            fmt.fmt.pix.bytesperline = min;
    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
            fmt.fmt.pix.sizeimage = min;

    switch (io)
    {
        case IO_METHOD_READ:
            init_read(fmt.fmt.pix.sizeimage);
            break;

        case IO_METHOD_MMAP:
            init_mmap();
            break;

        case IO_METHOD_USERPTR:
            init_userp(fmt.fmt.pix.sizeimage);
            break;
    }
}


static void close_device(void)
{
        if (-1 == close(fd))
                errno_exit("close");

        fd = -1;
}

static void open_device(void)
{
        struct stat st;

        if (-1 == stat(dev_name, &st)) {
            fprintf(stderr, "Cannot identify '%s': %d, %s\n",
                dev_name, errno, strerror(errno));

                exit(EXIT_FAILURE);
        }

        if (!S_ISCHR(st.st_mode)) {
                fprintf(stderr, "%s is no device\n", dev_name);
                
                exit(EXIT_FAILURE);
        }

        fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

        if (-1 == fd) {
                fprintf(stderr, "Cannot open '%s': %d, %s\n",
                    dev_name, errno, strerror(errno));

                exit(EXIT_FAILURE);
        }
}

static void usage(FILE *fp, int argc, char **argv)
{
    fprintf(fp,
        "Usage: %s [options]\n\n"
        "Version 1.3\n"
        "Options:\n"
        "-d | --device name   Video device name [%s]\n"
        "-h | --help          Print this message\n"
        "-m | --mmap          Use memory mapped buffers [default]\n"
        "-r | --read          Use read() calls\n"
        "-u | --userp         Use application allocated buffers\n"
        "-o | --output        Outputs stream to stdout\n"
        "-f | --format        Force format to 640x480 GREY\n"
        "-c | --count         Number of frames to grab [%i]\n"
        "",
        argv[0], dev_name, frame_count);
}

static const char short_options[] = "d:hmruofc:";

static const struct option
long_options[] = {
        { "device", required_argument, NULL, 'd' },
        { "help",   no_argument,       NULL, 'h' },
        { "mmap",   no_argument,       NULL, 'm' },
        { "read",   no_argument,       NULL, 'r' },
        { "userp",  no_argument,       NULL, 'u' },
        { "output", no_argument,       NULL, 'o' },
        { "format", no_argument,       NULL, 'f' },
        { "count",  required_argument, NULL, 'c' },
        { 0, 0, 0, 0 }
};

/*************************************************************************
 * Insert my code below
 *************************************************************************/

void get_cpu_core_config(void)
{
    cpu_set_t cpuset;
    pthread_t callingThread;
    int rc, idx;

    CPU_ZERO(&cpuset);

    // get affinity set for main thread
    callingThread = pthread_self();

    // Check the affinity mask assigned to the thread 
    rc = pthread_getaffinity_np(callingThread, sizeof(cpu_set_t), &cpuset);
    if (rc != EXIT_SUCCESS)
        perror("pthread_getaffinity_np");
    else
    {
        printf("thread running on CPU=%d, CPUs =", sched_getcpu());

        for (idx = 0; idx < CPU_SETSIZE; idx++)
            if (CPU_ISSET(idx, &cpuset))
                printf(" %d", idx);

        printf("\n");
    }

    printf("Using CPUs=%d from total available.\n", CPU_COUNT(&cpuset));
}

///< The value returned from the first call to getTimeMsec should be stored in global static start_time
double getTimeMsec(void)
{
    struct timespec event_ts = { 0, 0 };
    double event_time = 0;

    clock_gettime(CLOCK_REALTIME, &event_ts);
    event_time = ((event_ts.tv_sec) + ((event_ts.tv_nsec) / (double)NANOSEC_PER_SEC));
    return (event_time - start_time);
}

void print_scheduler(void)
{
    int schedType;

    schedType = sched_getscheduler(getpid());

    switch (schedType)
    {
    case SCHED_FIFO:
        printf("Pthread Policy is SCHED_FIFO\n");
        break;
    case SCHED_OTHER:
        printf("Pthread Policy is SCHED_OTHER\n");
        break;
    case SCHED_RR:
        printf("Pthread Policy is SCHED_RR\n");
        break;
    //case SCHED_DEADLINE:
        //printf("Pthread Policy is SCHED_DEADLINE\n");
        //break;
    default:
        printf("Pthread Policy is UNKNOWN\n");
    }
}

void* S0_sequencer(void* threadp)
{
    struct timespec delay_time = { 0, RTSEQ_DELAY_NSEC };
    struct timespec std_delay_time = { 0, RTSEQ_DELAY_NSEC };
    struct timespec current_time_val = { 0,0 };

    struct timespec remaining_time;
    double current_time, last_time, scaleDelay;
    double delta_t = (RTSEQ_DELAY_NSEC / (double)NANOSEC_PER_SEC);
    double scale_dt;
    int rc, delay_cnt = 0;
    unsigned long long seqCnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    current_time = getTimeMsec(); last_time = current_time - delta_t;

    syslog(LOG_CRIT, "FinalProject (S0_sequencer):                   start on CPU=%d @ sec=%lf after %lf with dt=%lf\n", sched_getcpu(), current_time, last_time, delta_t);

    do
    {
        current_time = getTimeMsec(); delay_cnt = 0;

#ifdef DRIFT_CONTROL
        scale_dt = (current_time - last_time) - delta_t;
        delay_time.tv_nsec = std_delay_time.tv_nsec - (scale_dt * (NANOSEC_PER_SEC + DT_SCALING_UNCERTAINTY_NANOSEC)) - CLOCK_BIAS_NANOSEC;
        //syslog(LOG_CRIT, "FinalProject (S0_sequencer):                   scale dt=%lf on CPU=%d @ sec=%lf after=%lf with dt=%lf\n", scale_dt, sched_getcpu(), current_time, last_time, delta_t);
#else
        delay_time = std_delay_time; scale_dt = delta_t;
#endif


#ifdef ABS_DELAY
        clock_gettime(MY_CLOCK, &current_time_val);
        delay_time.tv_sec = current_time_val.tv_sec;
        delay_time.tv_nsec = current_time_val.tv_nsec + delay_time.tv_nsec;

        if (delay_time.tv_nsec > NANOSEC_PER_SEC)
        {
            delay_time.tv_sec = delay_time.tv_sec + 1;
            delay_time.tv_nsec = delay_time.tv_nsec - NANOSEC_PER_SEC;
        }
        //syslog(LOG_CRIT, "FinalProject (S0_sequencer):                   cycle   %06llu on CPU=%d, delay for dt=%lf @ sec=%d, nsec=%d to sec=%d, nsec=%d\n", seqCnt, sched_getcpu(), scale_dt, current_time_val.tv_sec, current_time_val.tv_nsec, delay_time.tv_sec, delay_time.tv_nsec);
#endif


        // Delay loop with check for early wake-up
        do
        {
#ifdef ABS_DELAY
            rc = clock_nanosleep(MY_CLOCK, TIMER_ABSTIME, &delay_time, (struct timespec*)0);
#else
            rc = clock_nanosleep(MY_CLOCK, 0, &delay_time, &remaining_time);
#endif

            if (rc == EINTR)
            {
                syslog(LOG_CRIT, "FinalProject (S0_sequencer):                   EINTR @ sec=%lf\n", current_time);
                delay_cnt++;
            }
            else if (rc < EXIT_SUCCESS)
            {
                perror("FinalProject (S0_sequencer): nanosleep");
                exit(EXIT_FAILURE2);
            }

            //syslog(LOG_CRIT, "FinalProject (S0_sequencer): WOKE UP\n");

        } while (rc == EINTR);


        syslog(LOG_CRIT, "FinalProject (S0_sequencer):                   cycle   %06llu on CPU=%d @ sec=%lf, last=%lf, dt=%lf, sdt=%lf\n", seqCnt, sched_getcpu(), current_time, last_time, (current_time - last_time), scale_dt);

        // Release each service at a sub-rate of the generic sequencer rate

        // S1 Frame Acquisition
        if ((seqCnt % (int)(S0_FREQ/S1_FREQ)) == 0) sem_post(&semS1);

        // S2 Frame Difference Threshold
        if ((seqCnt % (int)(S0_FREQ/S2_FREQ)) == 0) sem_post(&semS2);

        // S3 Frame Select
        if ((seqCnt % (int)(S0_FREQ/S3_FREQ)) == 0) sem_post(&semS3);

        // S4 Frame Process
        if ((seqCnt % (int)(S0_FREQ/S4_FREQ)) == 0) sem_post(&semS4);

        // S5 Frame Write-Back
        if ((seqCnt % (int)(S0_FREQ/S5_FREQ)) == 0) sem_post(&semS5);

        seqCnt++;
        last_time = current_time;

    } while (!abortS0 && (seqCnt < threadParams->sequencePeriods));

    sem_post(&semS1); sem_post(&semS2); sem_post(&semS3); sem_post(&semS4); sem_post(&semS5);
    abortS1 = TRUE; abortS2 = TRUE; abortS3 = TRUE; abortS4 = TRUE; abortS5 = TRUE;

    pthread_exit((void*)0);
}

void* S1_frame_acquisition(void* threadp)
{
    double current_time;
    unsigned long long S1Cnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    current_time = getTimeMsec();
    syslog(LOG_CRIT, "FinalProject (S1_frame_acquisition):           start on CPU=%d @ sec=%lf\n", sched_getcpu(), current_time);

    while (!abortS1)
    {
        sem_wait(&semS1);
        S1Cnt++;

        current_time = getTimeMsec();
        syslog(LOG_CRIT, "FinalProject (S1_frame_acquisition):           release %06llu on CPU=%d @ sec=%lf\n", S1Cnt, sched_getcpu(), current_time);
    }

    pthread_exit((void*)0);
}

void* S2_frame_difference_threshold(void* threadp)
{
    double current_time;
    unsigned long long S2Cnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    current_time = getTimeMsec();
    syslog(LOG_CRIT, "FinalProject (S2_frame_difference_threshold):  start on CPU=%d @ sec=%lf\n", sched_getcpu(), current_time);

    while (!abortS2)
    {
        sem_wait(&semS2);
        S2Cnt++;

        current_time = getTimeMsec();
        syslog(LOG_CRIT, "FinalProject (S2_frame_difference_threshold):  release %06llu on CPU=%d @ sec=%lf\n", S2Cnt, sched_getcpu(), current_time);
    }

    pthread_exit((void*)0);
}

void* S3_frame_select(void* threadp)
{
    double current_time;
    unsigned long long S3Cnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    current_time = getTimeMsec();
    syslog(LOG_CRIT, "FinalProject (S3_frame_select):                start on CPU=%d @ sec=%lf\n", sched_getcpu(), current_time);

    while (!abortS3)
    {
        sem_wait(&semS3);
        S3Cnt++;

        current_time = getTimeMsec();
        syslog(LOG_CRIT, "FinalProject (S3_frame_select):                release %06llu on CPU=%d @ sec=%lf\n", S3Cnt, sched_getcpu(), current_time);
    }

    pthread_exit((void*)0);
}

void* S4_frame_process(void* threadp)
{
    double current_time;
    unsigned long long S4Cnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    current_time = getTimeMsec();
    syslog(LOG_CRIT, "FinalProject (S4_frame_process):               start on CPU=%d @ sec=%lf\n", sched_getcpu(), current_time);

    while (!abortS4)
    {
        sem_wait(&semS4);
        S4Cnt++;

        current_time = getTimeMsec();
        syslog(LOG_CRIT, "FinalProject (S4_frame_process):               release %06llu on CPU=%d @ sec=%lf\n", S4Cnt, sched_getcpu(), current_time);
    }

    pthread_exit((void*)0);
}

void* S5_frame_writeback(void* threadp)
{
    double current_time;
    unsigned long long S5Cnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    current_time = getTimeMsec();
    syslog(LOG_CRIT, "FinalProject (S5_frame_writeback):             start on CPU=%d @ sec=%lf\n", sched_getcpu(), current_time);

    while (!abortS5)
    {
        sem_wait(&semS5);
        S5Cnt++;

        current_time = getTimeMsec();
        syslog(LOG_CRIT, "FinalProject (S5_frame_writeback):             release %06llu on CPU=%d @ sec=%lf\n", S5Cnt, sched_getcpu(), current_time);
    }

    pthread_exit((void*)0);
}

/*************************************************************************
 * Insert my code above
 *************************************************************************/

int main(int argc, char **argv)
{
    if(argc > 1)
        dev_name = argv[1];
    else
        dev_name = "/dev/video0";

    for (;;)
    {
        int idx;
        int c;

        c = getopt_long(argc, argv,
                    short_options, long_options, &idx);

        if (-1 == c)
            break;

        switch (c)
        {
            case 0: /* getopt_long() flag */
                break;

            case 'd':
                dev_name = optarg;
                break;

            case 'h':
                usage(stdout, argc, argv);
                exit(EXIT_SUCCESS);

            case 'm':
                io = IO_METHOD_MMAP;
                break;

            case 'r':
                io = IO_METHOD_READ;
                break;

            case 'u':
                io = IO_METHOD_USERPTR;
                break;

            case 'o':
                out_buf++;
                break;

            case 'f':
                force_format++;
                break;

            case 'c':
                errno = 0;
                frame_count = strtol(optarg, NULL, 0);
                if (errno)
                        errno_exit(optarg);
                break;

            default:
                usage(stderr, argc, argv);
                exit(EXIT_FAILURE);
        }
    }

    // initialization of V4L2
    open_device();
    init_device();
    start_capturing();

    /*************************************************************************
     * Insert my code below
     *************************************************************************/
    char sys_buffer[SYS_BUF_SIZE];
    cpu_set_t allcpuset;
    cpu_set_t threadcpu;
    double current_time;
    int cpuidx;
    int i;
    int rc;
    int rt_max_prio;
    int rt_min_prio;
    struct timespec rt_res;

    start_time = getTimeMsec();

    ///< Delay for 1 sec before starting
    usleep(1000000);

    printf("Starting Synchronome Project...\n");
    get_cpu_core_config();

    ///< Initialize clock resolution
    clock_getres(MY_CLOCK, &rt_res);
    printf("RT clock resolution is %ld sec, %ld nsec\n", rt_res.tv_sec, rt_res.tv_nsec);

    printf("System has %d processors configured and %d available.\n", get_nprocs_conf(), get_nprocs());

    ///< Clear the CPU set then add all CPUs to entire CPU set
    CPU_ZERO(&allcpuset);
    for (i = 0; i < NUM_OF_CPU_CORES; i++)
        CPU_SET(i, &allcpuset);

    printf("Using CPUs=%d from total available.\n", CPU_COUNT(&allcpuset));

    ///< Grab and store min and max priorities from SCHED_FIFO scheduler
    rt_max_prio = sched_get_priority_max(SCHED_FIFO);
    rt_min_prio = sched_get_priority_min(SCHED_FIFO);

    rc = sched_getparam(getpid(), &main_param);
    main_param.sched_priority = rt_max_prio;

    if (rc < EXIT_SUCCESS) {
        perror("main_param");
    }

    print_scheduler();

    ///< Initialize all threads' scheduling policies, inheriting scheduler attributes, core affinities, and scheduling priorities
    for (i = 0; i < NUM_OF_THREADS; i++) {

        ///< Clear the current threads CPU set then assign it to CPU number RT_CORE
        CPU_ZERO(&threadcpu);
        cpuidx = (RT_CORE);
        CPU_SET(cpuidx, &threadcpu);

        ///< Initialize the scheduling attributes for this thread i
        rc = pthread_attr_init(&rt_sched_attr[i]);
        if (rc < EXIT_SUCCESS) {
            printf("Failed to initialize rt_sched_attr[%d]\n", i);
            exit(EXIT_FAILURE);
        }
        else {
            printf("Initialized rt_sched_attr[%d]\n", i);
        }

        ///< Set scheduling policies for this thread i
        rc = pthread_attr_setinheritsched(&rt_sched_attr[i], PTHREAD_EXPLICIT_SCHED);
        if (rc < EXIT_SUCCESS) {
            printf("Failed to set inherit sched for rt_sched_attr[%d]\n", i);
            exit(EXIT_FAILURE);
        }
        else {
            printf("Set inherit sched for rt_sched_attr[%d] to PTHREAD_EXPLICIT_SCHED\n", i);
        }
        rc = pthread_attr_setschedpolicy(&rt_sched_attr[i], SCHED_FIFO);
        if (rc < EXIT_SUCCESS) {
            printf("Failed to set scheduling policy for rt_sched_attr[%d] to SCHED_FIFO\n", i);
            exit(EXIT_FAILURE);
        }
        else {
            printf("Set scheduling policy for rt_sched_attr[%d] to SCHED_FIFO\n", i);
        }

        ///< Set core affinity for this thread i
        rc = pthread_attr_setaffinity_np(&rt_sched_attr[i], sizeof(cpu_set_t), &threadcpu);
        if (rc < EXIT_SUCCESS) {
            printf("Failed to set core affinity for rt_sched_attr[%d] to Core %d\n", i, cpuidx);
            exit(EXIT_FAILURE);
        }
        else {
            printf("Set core affinity for rt_sched_attr[%d] to Core %d\n", i, cpuidx);
        }

        ///< Set scheduling priority for this thread i (higher priority services for lower values of i
        rt_param[i].sched_priority = rt_max_prio - i;
        rc = pthread_attr_setschedparam(&rt_sched_attr[i], &rt_param[i]);
        if (rc < EXIT_SUCCESS) {
            printf("Failed to set scheduling priority for rt_sched_attr[%d] to %d/%d\n", i, rt_param[i].sched_priority, rt_max_prio);
            exit(EXIT_FAILURE);
        }
        else {
            printf("Set scheduling priority for rt_sched_attr[%d] to %d/%d\n", i, rt_param[i].sched_priority, rt_max_prio);
        }

        ///< Pass number of S0_PERIODS to S0 Sequencer so we know how long to run
        if (i == 0) {
            threadParams[i].sequencePeriods = S0_PERIODS;
        }

        ///< Store thread IDs of each service based on values of i
        threadParams[i].threadIdx = i;
    }

    printf("Service threads will run on %d CPU cores\n", CPU_COUNT(&threadcpu));
    current_time = getTimeMsec();
    syslog(LOG_CRIT, "FinalProject (MAIN): on CPU=%d @ sec=%lf, elapsed=%lf\n", sched_getcpu(), start_time, current_time);

    ///< create frame acquisition thread but perform no action because semaphore is not given
    rc = pthread_create(&threads[1],                    // pointer to thread descriptor
                        &rt_sched_attr[1],              // use specific attributes
                        S1_frame_acquisition,           // thread function entry point
                        (void*)&(threadParams[1])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[1]\n");
        exit(EXIT_FAILURE);
    }

    ///< create frame difference threshold thread but perform no action because semaphore is not given
    rc = pthread_create(&threads[2],                    // pointer to thread descriptor
                        &rt_sched_attr[2],              // use specific attributes
                        S2_frame_difference_threshold,  // thread function entry point
                        (void*)&(threadParams[2])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[2]\n");
        exit(EXIT_FAILURE);
    }

    ///< create frame select thread but perform no action because semaphore is not given
    rc = pthread_create(&threads[3],                    // pointer to thread descriptor
                        &rt_sched_attr[3],              // use specific attributes
                        S3_frame_select,                // thread function entry point
                        (void*)&(threadParams[3])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[3]\n");
        exit(EXIT_FAILURE);
    }

    ///< create frame process thread but perform no action because semaphore is not given
    rc = pthread_create(&threads[4],                    // pointer to thread descriptor
                        &rt_sched_attr[4],              // use specific attributes
                        S4_frame_process,               // thread function entry point
                        (void*)&(threadParams[4])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[4]\n");
        exit(EXIT_FAILURE);
    }

    ///< create frame write-back thread but perform no action because semaphore is not given
    rc = pthread_create(&threads[5],                    // pointer to thread descriptor
                        &rt_sched_attr[5],              // use specific attributes
                        S5_frame_writeback,             // thread function entry point
                        (void*)&(threadParams[5])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[5]\n");
        exit(EXIT_FAILURE);
    }

    ///< create sequencer thread last because now all other S1-S5 are created and ready to fire off
    rc = pthread_create(&threads[0],                    // pointer to thread descriptor
        &rt_sched_attr[0],              // use specific attributes
        S0_sequencer,                   // thread function entry point
        (void*)&(threadParams[0])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[0]\n");
        exit(EXIT_FAILURE);
    }

    ///< Join back all threads
    for (i = 0; i < NUM_OF_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // service loop frame read
    // mainloop();

    /*************************************************************************
     * Insert my code above
     *************************************************************************/

    // shutdown of frame acquisition service
    stop_capturing();
    uninit_device();
    close_device();

    fprintf(stderr, "\n");

    printf("Ending Synchronome Project... writing syslog trace to ./syslog_trace_%02dmin.txt\n\n", S0_RUN_TIME_MIN);

    ///< Calculate number of lines to tail from syslog in relation to S0_RUN_TIME_MIN
    //sprintf(sys_buffer, "tail -%d /var/log/syslog | grep -n FinalProject > ./syslog_trace_%02dmin.txt", (int)((115.0/96.0)*S0_PERIODS + 13), S0_RUN_TIME_MIN);
    //sprintf(sys_buffer, "tail -%d /var/log/syslog | grep -n FinalProject > ./syslog_trace_%02dmin.txt", 75000, S0_RUN_TIME_MIN);

    ///< Delay for 1 sec before checking syslog trace
    usleep(1000000);

    //system(sys_buffer);

    printf("Use below commands to manually view & generate syslog_trace_manual.txt for this session:\n");
    printf("\t1. tail -X /var/log/syslog | grep -n FinalProject\n");
    printf("\t\t- Estimate X based on runtime. For example, X is ~25888 for 3 min and ~258754 for 30 min\n");
    printf("\t\t- This is used to grab start + end line numbers of trace to store into file based on trace times\n");
    printf("\t2. tail -Y /var/log/syslog > ./syslog_trace_manual.txt | grep -n FinalProject\n");
    printf("\t\t- Where Y is the exact beginning line number to begin storing into txt file\n");
    printf("\n");

    return 0;
}
