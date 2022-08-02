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
#include <signal.h>
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
#define EXIT_FAILURE_N (-1)

///< Conversion values
#define NANOSEC_PER_SEC (1000000000)
#define NANOSEC_PER_MICROSEC (1000)
#define MICROSEC_PER_SEC (1000000)
#define MILLISEC_PER_SEC (1000)
#define SEC_PER_MIN (60)

///< Number of RT services requiring their own threads
///< - NOTE: S0 - Sequencer WILL NOT REQUIRE ITS OWN THREAD
///< S1 - Frame Acquisition for capturing frames of clock
///< S2 - Frame Difference Threshold for matrix subtraction to see if seconds hand has moved
///< S3 - Frame Select for selecting the "best" frame to send to Frame Write-Back buffer
///< S4 - Frame Process for processing the frame (in my case, to grayscale)
///< S5 - Frame Write-Back for writing the processed frame from buffer to FLASH
#define NUM_OF_THREADS 5
#define S1 0
#define S2 1
#define S3 2
#define S4 3
#define S5 4

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

///< Resolution of pictures captured by S1 Frame Acquisition
#define PHOTO_RES (1280*960)

///< Store 60 seconds of data at 20 Hz
#define BIGBUFFER_READ_MAX_NUM_OF_FRAMES_STORED (S0_RUN_TIME_SEC*S1_FREQ)

///< Same size as bigbuffer_read
#define BIGBUFFER_DIFF_THRESHOLD_MAX_NUM_OF_FRAMES_STORED (BIGBUFFER_READ_MAX_NUM_OF_FRAMES_STORED)

///< Store 60 seconds of data at 1 Hz + 1 extra frame
#define BIGBUFFER_SELECT_MAX_NUM_OF_FRAMES_STORED (S0_RUN_TIME_SEC*S3_FREQ) + 1

///< Number of frames expected at the end of the test
///< Example (1) - Running for 1800 sec for 1 Hz synchronome will be (S0_RUN_TIME_SEC)*(S3_FREQ) + Initial Frames = (1800 sec)*(1 Hz) + 9 Initial Frames = 1809 frames
///<               Thus, for 1800 sec at 1 Hz selection this would be set to 1809
///< Example (2) - Running for 180 sec for 10 Hz synchronome will be (S0_RUN_TIME_SEC)*(S3_FREQ) + Initial Frames = (180 sec)*(10 Hz) + 9 Initial Frames = 1809 frames
///<               Thus, for 180 sec at 10 Hz selection this would be set to 1809
#define FRAME_COUNT ((S0_RUN_TIME_SEC)*(S3_FREQ) + 9)

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
sem_t sem[NUM_OF_THREADS];

///< Keep track of global times
double start_realtime;
double end_realtime;
struct timespec start_time_val;

///< Controls for aborting all services S0-S5
int abort_test = FALSE;
int abort_threads[NUM_OF_THREADS];

///< Counter for S0 Sequencer
static unsigned long long seqCnt = 0;

///< timer + timer specs for S0 Sequencer
static timer_t timer_1;
static struct itimerspec itime = { {1,0}, {1,0} };
static struct itimerspec last_itime;

///< Store S0_PERIODS into unsigned long long
unsigned long long sequencePeriods;

///< Buffer info for S1 Frame Acquisition
unsigned char bigbuffer_read[PHOTO_RES*BIGBUFFER_READ_MAX_NUM_OF_FRAMES_STORED];
static int bigbuffer_read_i = 0;
void* p_buf[BIGBUFFER_READ_MAX_NUM_OF_FRAMES_STORED];
static int p_buf_i = 0;
int size_buf[BIGBUFFER_READ_MAX_NUM_OF_FRAMES_STORED];
static int size_buf_i = 0;

///< Counter for amount of frames read by S1 Frame Acquisition. Always ignore the first 8 frames
int framecnt_read = -8;

///< Buffer info for S2 Frame Difference Threshold
enum frame_quality {untouched, stable, blurry};
enum frame_quality bigbuffer_diff_threshold[BIGBUFFER_DIFF_THRESHOLD_MAX_NUM_OF_FRAMES_STORED];

///< Counter for amount of frames marked by S2 Frame Difference Threshold
int framecnt_diff_threshold = 0;
int framecnt_diff_threshold_first;
int framecnt_diff_threshold_last;

///< Buffer info for S3 Frame Select
unsigned char bigbuffer_select[PHOTO_RES*BIGBUFFER_SELECT_MAX_NUM_OF_FRAMES_STORED];
int size_buf_select[BIGBUFFER_SELECT_MAX_NUM_OF_FRAMES_STORED];

///< Counter for amount of frames selected by S3 Frame Select
int framecnt_select = 0;
int framecnt_select_first;
int framecnt_select_last;

///< Frame number selected from bigbuffer_read by S3 Frame Select
int frame_selected_from_bigbuffer_read = 0;

///< Counter for amount of frames read by S4 Frame Process
int framecnt_process = 0;

///< 1 photo buffer
//struct v4l2_buffer buf_read;

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
    void* start;
    size_t  length;
};

static char* dev_name;
//static enum io_method   io = IO_METHOD_USERPTR;
//static enum io_method   io = IO_METHOD_READ;
static enum io_method   io = IO_METHOD_MMAP;
static int              fd = -1;
struct buffer* buffers;
static unsigned int     n_buffers;
static int              out_buf;
static int              force_format = 1;
static int              frame_count = (FRAME_COUNT);

// always ignore first 8 frames
int framecnt = -8;

unsigned char bigbuffer[PHOTO_RES];

static void errno_exit(const char* s)
{
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));

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

    //printf("wrote %d bytes\n", total);
    syslog(LOG_INFO, "FinalProject (S5_frame_writeback):             Wrote %04d %d bytes\n", tag, total);

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

    //printf("wrote %d bytes\n", total);
    syslog(LOG_INFO, "FinalProject (S5_frame_writeback):             Wrote %04d %d bytes\n", tag, total);

    close(dumpfd);

}

static void store_buf_read(const void* p, int size, unsigned int tag, struct timespec* time)
{
    if (framecnt_read >= 0) {
        int i;
        unsigned char* pptr = (unsigned char*)p;

        for (i = 0; i < size; i = i + 1) {
            bigbuffer_read[(PHOTO_RES*framecnt_read) + i] = pptr[i];
        }
    }
}

static void store_buf_select(const void* p, int size, unsigned int tag, struct timespec* time)
{
    if (framecnt_select >= 0) {
        int i;
        unsigned char* pptr = (unsigned char*)p;

        for (i = 0; i < size; i = i + 1) {
            //syslog(LOG_INFO, "FinalProject (S3_frame_select): i=%d, size=%d, framecnt_select=%d", i, size, framecnt_select);
            bigbuffer_select[(PHOTO_RES*framecnt_select) + i] = pptr[PHOTO_RES*frame_selected_from_bigbuffer_read + i];
        }
    }
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

static void process_image(const void* p, int size)
{
    int i, newi, newsize = 0;
    struct timespec frame_time;
    int y_temp, y2_temp, u_temp, v_temp;
    unsigned char* pptr = (unsigned char*)p;

    // record when process was called
    clock_gettime(MY_CLOCK, &frame_time);

    framecnt++;
    framecnt_process++;

    //printf("frame %d: ", framecnt);
    syslog(LOG_INFO, "FinalProject (S4_frame_process):               frame %d: , frame_process %d:", framecnt, framecnt_process);

    // This just dumps the frame to a file now, but you could replace with whatever image
    // processing you wish.
    //

    if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_GREY)
    {
        //printf("Dump graymap as-is size %d\n", size);
        syslog(LOG_INFO, "FinalProject (S4_frame_process):               Dump graymap as-is size %d\n", size);

        dump_pgm(p, size, framecnt, &frame_time);
        //dump_pgm(p, size, framecnt_process, &frame_time);
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
            //dump_ppm(bigbuffer, ((size * 6) / 4), framecnt_process, &frame_time);

            //printf("Dump YUYV converted to RGB size %d\n", size);
            syslog(LOG_INFO, "FinalProject (S4_frame_process):               Dump YUYV converted to RGB size %d\n", size);

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
            //dump_pgm(bigbuffer, (size / 2), framecnt_process, &frame_time);

            //printf("Dump YUYV converted to YY size %d\n", size);
            syslog(LOG_INFO, "FinalProject (S4_frame_process):               Dump YUYV converted to YY size %d\n", size);
        }
#endif

    }

    else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB24)
    {
        //printf("Dump RGB as-is size %d\n", size);
        syslog(LOG_INFO, "FinalProject (S4_frame_process):               Dump RGB as-is size %d\n", size);

        dump_ppm(p, size, framecnt, &frame_time);
        //dump_ppm(p, size, framecnt_process, &frame_time);
    }
    else
    {
        //printf("ERROR - unknown dump format\n");
        syslog(LOG_ERR, "FinalProject (S4_frame_process):               ERROR - unknown dump format\n");
    }

    fflush(stderr);

    //fprintf(stderr, ".");
    //syslog(LOG_ERR, ".");

    fflush(stdout);
}

static void mark_frames(void) {
    int i;

    ///< Get current range of frames we need to mark
    framecnt_diff_threshold_first = framecnt_diff_threshold;
    framecnt_diff_threshold_last = framecnt_read;
    syslog(LOG_INFO, "FinalProject (S2_frame_difference_threshold): framecnt_diff_threshold_first=%d, framecnt_diff_threshold_last=%d", framecnt_diff_threshold_first, framecnt_diff_threshold_last);
    ///< For testing, mark only first + last frames as stable
    for (i = framecnt_diff_threshold_first; i < framecnt_diff_threshold_last; i++) {
        if (i == framecnt_diff_threshold_first) {
            bigbuffer_diff_threshold[i] = stable;
        }
        else {
            bigbuffer_diff_threshold[i] = blurry;
        }

        ///< We have stored another frame_quality
        framecnt_diff_threshold++;
    }
}

static void select_frame(void) {
    int i;
    int first;
    int last;
    struct timespec frame_time;

    ///< Get current range of frames we need to select from
    framecnt_select_first = framecnt_select;
    framecnt_select_last = framecnt_diff_threshold;
    syslog(LOG_INFO, "FinalProject (S3_frame_select): framecnt_select_first=%d, framecnt_select_last=%d", framecnt_select_first, framecnt_select_last);

    ///< For testing, select last frame read by S1_frame_acquisition
    frame_selected_from_bigbuffer_read = framecnt_read - 1;

    if (frame_selected_from_bigbuffer_read >= 0) {
        clock_gettime(MY_CLOCK, &frame_time);

        ///< Store the size of selected frame (in bytes) into global select buffer
        size_buf_select[framecnt_select] = size_buf[frame_selected_from_bigbuffer_read];

        ///< Now store the frame into global select buffer
        store_buf_select(bigbuffer_read, size_buf[frame_selected_from_bigbuffer_read], framecnt_select, &frame_time);

        //syslog(LOG_INFO, "FinalProject (S3_frame_select): frame_selected_from_bigbuffer_read=%d, framecnt_read=%d, framecnt_select=%d", frame_selected_from_bigbuffer_read, framecnt_read, framecnt_select);

        ///< We have stored another selected frame
        framecnt_select++;
    }

    ///< For testing, store all stable frames into bigbuffer_select
    //for (i = first; i < last; i++) {
    //    if (bigbuffer_diff_threshold[i] == stable) {
    //        clock_gettime(MY_CLOCK, &frame_time);
    //
    //        ///< Store the size of selected frame (in bytes) into global select buffer
    //        size_buf_select[framecnt_select] = size_buf[i];
    //
    //        ///< Now store the frame into global select buffer
    //        store_buf_select(bigbuffer_read[i], size_buf[i], framecnt_select, &frame_time);
    //
    //        ///< We have stored another selected frame
    //        framecnt_select++;
    //    }
    //}
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
                //printf("mmap failure\n");
                //syslog(LOG_INFO, "mmap failure\n");

                errno_exit("VIDIOC_DQBUF");
            }
        }

        assert(buf.index < n_buffers);

        //process_image(buffers[buf.index].start, buf.bytesused);

        ///< Declare and grab the time frame has been read
        struct timespec frame_time;
        clock_gettime(MY_CLOCK, &frame_time);

        ///< Store the size of frame (in bytes) into global read buffer
        size_buf[framecnt_read] = buf.bytesused;

        ///< Now store the frame into global read buffer
        store_buf_read(buffers[buf.index].start, buf.bytesused, framecnt_read, &frame_time);

        ///< We have stored another read frame
        framecnt_read++;

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

    //printf("R");
    //syslog(LOG_INFO, "R");

    return 1;
}

static void mainloop(void)
{
    unsigned int count;
    //struct timespec read_delay;
    struct timespec time_error;

    // Replace this with a sequencer DELAY
    //
    // 250 million nsec is a 250 msec delay, for 4 fps
    // 1 sec for 1 fps
    //
    //read_delay.tv_sec = 1;
    //read_delay.tv_nsec = (1.0/S1_FREQ)*(NANOSEC_PER_SEC);

    ///< S1 Frame Acquisition will invoke mainloop() at S1_FREQ Hz. Upon each invocation, we should only read 1 frame
    //count = frame_count;
    count = 1;

    while (count > 0)
    {
        for (;;)
        {
            fd_set fds;
            struct timeval tv;
            int r;

            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            ///< Don't let select block for more than S1 Frame Acquisition's period
            tv.tv_sec = 0;
            tv.tv_usec = (1.0/S1_FREQ)*(MICROSEC_PER_SEC);

            r = select(fd + 1, &fds, NULL, NULL, &tv);

            if (-1 == r)
            {
                if (EINTR == errno)
                    continue;
                errno_exit("select");
            }

            if (0 == r)
            {
                //fprintf(stderr, "select timeout\n");
                syslog(LOG_ERR, "FinalProject (S1_frame_acquisition):           select timeout\n");

                exit(EXIT_FAILURE);
            }

            ///< If we have successfully read a frame, exit the loop
            if (read_frame())
            {
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
        }
        else
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

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buffers;

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

    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s does not support "
                "user pointer i/o\n", dev_name);

            exit(EXIT_FAILURE);
        }
        else {
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
        syslog(LOG_INFO, "FinalProject (MAIN):                           FORCING FORMAT\n");

        fmt.fmt.pix.width = HRES;
        fmt.fmt.pix.height = VRES;

        // Specify the Pixel Coding Formate here

        // This one works for Logitech C200
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

        //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
        //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_VYUY;

        // Would be nice if camera supported
        //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
        //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;

        //fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

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

static void usage(FILE* fp, int argc, char** argv)
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

    clock_gettime(MY_CLOCK, &event_ts);
    return ((event_ts.tv_sec) * 1000.0) + ((event_ts.tv_nsec) / 1000000.0);
}

double realtime(struct timespec* tsptr)
{
    return ((double)(tsptr->tv_sec) + (((double)tsptr->tv_nsec) / 1000000000.0));
}

void print_scheduler(void)
{
    int schedType;

    schedType = sched_getscheduler(getpid());

    switch (schedType)
    {
    case SCHED_FIFO:
        printf("Pthread Policy is SCHED_FIFO on CPU=%d\n", sched_getcpu());
        break;
    case SCHED_OTHER:
        printf("Pthread Policy is SCHED_OTHER on CPU=%d\n", sched_getcpu());
        break;
    case SCHED_RR:
        printf("Pthread Policy is SCHED_RR on CPU=%d\n", sched_getcpu());
        break;
        //case SCHED_DEADLINE:
            //printf("Pthread Policy is SCHED_DEADLINE on CPU=%d\n", sched_getcpu());
            //break;
    default:
        printf("Pthread Policy is UNKNOWN on CPU=%d\n", sched_getcpu());
    }
}

void* S0_sequencer(void* threadp)
{
    struct timespec current_time_val;
    double current_realtime;
    int rc, flags = 0;
    int i;

    ///< Entering here means we received interval timer signal

    seqCnt++;

    //clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime=realtime(&current_time_val);
    //printf("Sequencer on core %d for cycle %llu @ sec=%6.9lf\n", sched_getcpu(), seqCnt, current_realtime-start_realtime);
    //syslog(LOG_CRIT, "Sequencer on core %d for cycle %llu @ sec=%6.9lf\n", sched_getcpu(), seqCnt, current_realtime-start_realtime);

    ///< Post semaphore for S1 Frame Acquisition at a derivative frequency of S0 Sequencer
    if ((seqCnt % (int)(S0_FREQ/S1_FREQ)) == 0) {
        sem_post(&sem[0]);
    }

    ///< Post semaphore for S2 Frame Difference Threshold at a derivative frequency of S0 Sequencer
    if ((seqCnt % (int)(S0_FREQ/S2_FREQ)) == 0) {
        sem_post(&sem[1]);
    }

    ///< Post semaphore for S3 Frame Selection at a derivative frequency of S0 Sequencer
    if ((seqCnt % (int)(S0_FREQ/S3_FREQ)) == 0) {
        sem_post(&sem[2]);
    }

    ///< Post semaphore for S4 Frame Process at a derivative frequency of S0 Sequencer
    if ((seqCnt % (int)(S0_FREQ/S4_FREQ)) == 0) {
        sem_post(&sem[3]);
    }

    ///< Post semaphore for S5 Frame Write-Back at a derivative frequency of S0 Sequencer
    if ((seqCnt % (int)(S0_FREQ/S5_FREQ)) == 0) {
        sem_post(&sem[4]);
    }

    if (abort_test || (seqCnt >= sequencePeriods))
    {
        ///< Disable interval timer
        itime.it_interval.tv_sec = 0;
        itime.it_interval.tv_nsec = 0;
        itime.it_value.tv_sec = 0;
        itime.it_value.tv_nsec = 0;
        timer_settime(timer_1, flags, &itime, &last_itime);
        printf("Disabling S0 Sequencer interval timer with abort=%d and Sequence Count %llu of Sequence Period %lld\n", abort_test, seqCnt, sequencePeriods);

        ///< Shutdown all services
        for (i = 0; i < NUM_OF_THREADS; i++) {
            sem_post(&sem[i]);
            abort_threads[i] = TRUE;
        }
    }

}

void* S1_frame_acquisition(void* threadp)
{
    struct timespec current_time_val;
    double current_realtime;
    unsigned long long S1Cnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    ///< Start up processing and resource initialization
    clock_gettime(MY_CLOCK, &current_time_val);
    current_realtime = realtime(&current_time_val);

    syslog(LOG_CRIT, "FinalProject (S1_frame_acquisition):           start on CPU=%d @ sec=%lf\n", sched_getcpu(), current_realtime - start_realtime);

    while (!abort_threads[S1])
    {
        sem_wait(&sem[S1]);
        S1Cnt++;

        ///< Call mainloop() at S1_FREQ Hz to read exactly 1 frame from camera into bigbuffer_read[]
        mainloop();

        clock_gettime(MY_CLOCK, &current_time_val);
        current_realtime = realtime(&current_time_val);

        syslog(LOG_CRIT, "FinalProject (S1_frame_acquisition):           release %06llu on CPU=%d @ sec=%lf\n", S1Cnt, sched_getcpu(), current_realtime - start_realtime);
    }

    pthread_exit((void*)0);
}

void* S2_frame_difference_threshold(void* threadp)
{
    struct timespec current_time_val;
    double current_realtime;
    unsigned long long S2Cnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    ///< Start up processing and resource initialization
    clock_gettime(MY_CLOCK, &current_time_val);
    current_realtime = realtime(&current_time_val);

    syslog(LOG_CRIT, "FinalProject (S2_frame_difference_threshold):  start on CPU=%d @ sec=%lf\n", sched_getcpu(), current_realtime - start_realtime);

    while (!abort_threads[S2])
    {
        sem_wait(&sem[S2]);
        S2Cnt++;

        ///< Out of 1/S2_FREQ sec, mark which frames are blurry and which are stable
        mark_frames();

        clock_gettime(MY_CLOCK, &current_time_val);
        current_realtime = realtime(&current_time_val);

        syslog(LOG_CRIT, "FinalProject (S2_frame_difference_threshold):  release %06llu on CPU=%d @ sec=%lf\n", S2Cnt, sched_getcpu(), current_realtime - start_realtime);
    }

    pthread_exit((void*)0);
}

void* S3_frame_select(void* threadp)
{
    struct timespec current_time_val;
    double current_realtime;
    unsigned long long S3Cnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    ///< Start up processing and resource initialization
    clock_gettime(MY_CLOCK, &current_time_val);
    current_realtime = realtime(&current_time_val);

    syslog(LOG_CRIT, "FinalProject (S3_frame_select):                start on CPU=%d @ sec=%lf\n", sched_getcpu(), current_realtime - start_realtime);

    while (!abort_threads[S3])
    {
        sem_wait(&sem[S3]);
        S3Cnt++;

        ///< Select a frame every 1 sec based on its markings from S2 Frame Difference Threshold
        select_frame();

        clock_gettime(MY_CLOCK, &current_time_val);
        current_realtime = realtime(&current_time_val);

        syslog(LOG_CRIT, "FinalProject (S3_frame_select):                release %06llu on CPU=%d @ sec=%lf\n", S3Cnt, sched_getcpu(), current_realtime - start_realtime);
    }

    pthread_exit((void*)0);
}

void* S4_frame_process(void* threadp)
{
    struct timespec current_time_val;
    double current_realtime;
    unsigned long long S4Cnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    ///< Start up processing and resource initialization
    clock_gettime(MY_CLOCK, &current_time_val);
    current_realtime = realtime(&current_time_val);

    syslog(LOG_CRIT, "FinalProject (S4_frame_process):               start on CPU=%d @ sec=%lf\n", sched_getcpu(), current_realtime - start_realtime);

    while (!abort_threads[S4])
    {
        sem_wait(&sem[S4]);
        S4Cnt++;

        clock_gettime(MY_CLOCK, &current_time_val);
        current_realtime = realtime(&current_time_val);

        syslog(LOG_CRIT, "FinalProject (S4_frame_process):               release %06llu on CPU=%d @ sec=%lf\n", S4Cnt, sched_getcpu(), current_realtime - start_realtime);
    }

    pthread_exit((void*)0);
}

void* S5_frame_writeback(void* threadp)
{
    struct timespec current_time_val;
    double current_realtime;
    unsigned long long S5Cnt = 0;
    threadParams_t* threadParams = (threadParams_t*)threadp;

    ///< Start up processing and resource initialization
    clock_gettime(MY_CLOCK, &current_time_val);
    current_realtime = realtime(&current_time_val);

    syslog(LOG_CRIT, "FinalProject (S5_frame_writeback):             start on CPU=%d @ sec=%lf\n", sched_getcpu(), current_realtime - start_realtime);

    while (!abort_threads[S5])
    {
        sem_wait(&sem[S5]);
        S5Cnt++;

        //dump_ppm(p, size, framecnt, &frame_time);

        clock_gettime(MY_CLOCK, &current_time_val);
        current_realtime = realtime(&current_time_val);

        syslog(LOG_CRIT, "FinalProject (S5_frame_writeback):             release %06llu on CPU=%d @ sec=%lf\n", S5Cnt, sched_getcpu(), current_realtime - start_realtime);
    }

    pthread_exit((void*)0);
}

/*************************************************************************
 * Insert my code above
 *************************************************************************/

int main(int argc, char** argv)
{
    if (argc > 1)
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
    int flags = 0;
    int i;
    int rc;
    int rt_max_prio;
    int rt_min_prio;
    struct timespec current_time_val;
    struct timespec current_time_res;
    double current_realtime;
    double current_realtime_res;

    clock_gettime(MY_CLOCK, &start_time_val);
    start_realtime = realtime(&start_time_val);
    clock_gettime(MY_CLOCK, &current_time_val);
    current_realtime = realtime(&current_time_val);

    ///< Delay for 1 sec before starting
    usleep(1000000);

    printf("Starting Synchronome Project...\n");
    printf("Main");
    get_cpu_core_config();

    ///< Initialize diff threshold buffer to all untouched
    for (i = 0; i < BIGBUFFER_DIFF_THRESHOLD_MAX_NUM_OF_FRAMES_STORED; i++) {
        bigbuffer_diff_threshold[i] = untouched;
    }

    ///< Initialize clock resolution
    clock_getres(MY_CLOCK, &current_time_res);
    current_realtime_res = realtime(&current_time_res);
    printf("Starting S0 Sequencer @ sec=%6.9lf with resolution at %6.9lf sec\n", (current_realtime - start_realtime), current_realtime_res);

    printf("System has %d processors configured and %d available.\n", get_nprocs_conf(), get_nprocs());

    ///< Clear the CPU set then add all CPUs to entire CPU set
    CPU_ZERO(&allcpuset);
    for (i = 0; i < NUM_OF_CPU_CORES; i++) {
        CPU_SET(i, &allcpuset);
    }

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

        ///< Initialize semaphore for this thread i
        rc = sem_init(&sem[i], 0, 0);
        if (rc > EXIT_SUCCESS) {
            printf("Failed to initialize sem[%d]\n", i);
            exit(EXIT_FAILURE_N);
        }
        else {
            printf("sem[%d] initialized\n", i);
        }

        ///< Initialize abort signal for this thread i
        abort_threads[i] = FALSE;

        ///< Initialize the scheduling attributes for this thread i
        rc = pthread_attr_init(&rt_sched_attr[i]);
        if (rc < EXIT_SUCCESS) {
            printf("Failed to initialize rt_sched_attr[%d]\n", i);
            exit(EXIT_FAILURE);
        }
        else {
            printf("rt_sched_attr[%d] initialized\n", i);
        }

        ///< Set scheduling policies for this thread i
        rc = pthread_attr_setinheritsched(&rt_sched_attr[i], PTHREAD_EXPLICIT_SCHED);
        if (rc < EXIT_SUCCESS) {
            printf("Failed to set inherit sched for rt_sched_attr[%d]\n", i);
            exit(EXIT_FAILURE);
        }
        else {
            printf("rt_sched_attr[%d] inherit sched set to PTHREAD_EXPLICIT_SCHED\n", i);
        }
        rc = pthread_attr_setschedpolicy(&rt_sched_attr[i], SCHED_FIFO);
        if (rc < EXIT_SUCCESS) {
            printf("Failed to set scheduling policy for rt_sched_attr[%d] to SCHED_FIFO\n", i);
            exit(EXIT_FAILURE);
        }
        else {
            printf("rt_sched_attr[%d] scheduling policy set to SCHED_FIFO\n", i);
        }

        ///< Set core affinity for this thread i
        rc = pthread_attr_setaffinity_np(&rt_sched_attr[i], sizeof(cpu_set_t), &threadcpu);
        if (rc < EXIT_SUCCESS) {
            printf("Failed to set core affinity for rt_sched_attr[%d] to %d\n", i, cpuidx);
            exit(EXIT_FAILURE);
        }
        else {
            printf("rt_sched_attr[%d] core affinity set to %d\n", i, cpuidx);
        }

        ///< Set scheduling priority for this thread i (higher priority services for lower values of i
        rt_param[i].sched_priority = rt_max_prio - i;
        rc = pthread_attr_setschedparam(&rt_sched_attr[i], &rt_param[i]);
        if (rc < EXIT_SUCCESS) {
            printf("Failed to set scheduling priority for rt_sched_attr[%d] to %d/%d\n", i, rt_param[i].sched_priority, rt_max_prio);
            exit(EXIT_FAILURE);
        }
        else {
            printf("rt_sched_attr[%d] scheduling priority set to %d/%d\n", i, rt_param[i].sched_priority, rt_max_prio);
        }

        ///< Store thread IDs of each service based on values of i
        threadParams[i].threadIdx = i;
    }

    printf("Service threads will run on %d CPU cores\n", CPU_COUNT(&threadcpu));
    clock_gettime(MY_CLOCK, &current_time_val);
    current_realtime = realtime(&current_time_val);
    syslog(LOG_CRIT, "FinalProject (MAIN):                           on CPU=%d @ sec=%lf\n", sched_getcpu(), current_realtime - start_realtime);

    ///< Create S1 Frame Acquisition thread but perform no action because semaphore is not given
    rc = pthread_create(&threads[S1],                    // pointer to thread descriptor
                        &rt_sched_attr[S1],              // use specific attributes
                        S1_frame_acquisition,           // thread function entry point
                        (void*)&(threadParams[S1])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[%d] tied to S1 Frame Acquisition\n", S1);
        exit(EXIT_FAILURE);
    }
    else {
        printf("Created thread[%d] tied to S1 Frame Acquisition\n", S1);
    }

    ///< Create S2 Frame Difference Threshold thread but perform no action because semaphore is not given
    rc = pthread_create(&threads[S2],                    // pointer to thread descriptor
                        &rt_sched_attr[S2],              // use specific attributes
                        S2_frame_difference_threshold,  // thread function entry point
                        (void*)&(threadParams[S2])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[%d] tied to S2 Frame Difference Threshold\n", S2);
        exit(EXIT_FAILURE);
    }
    else {
        printf("Created thread[%d] tied to S2 Frame Difference Threshold\n", S2);
    }

    ///< Create S3 Frame Select thread but perform no action because semaphore is not given
    rc = pthread_create(&threads[S3],                    // pointer to thread descriptor
                        &rt_sched_attr[S3],              // use specific attributes
                        S3_frame_select,                // thread function entry point
                        (void*)&(threadParams[S3])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[%d] tied to S3 Frame Select\n", S3);
        exit(EXIT_FAILURE);
    }
    else {
        printf("Created thread[%d] tied to S3 Frame Select\n", S3);
    }

    ///< Create S4 Frame Process thread but perform no action because semaphore is not given
    rc = pthread_create(&threads[S4],                    // pointer to thread descriptor
                        &rt_sched_attr[S4],              // use specific attributes
                        S4_frame_process,               // thread function entry point
                        (void*)&(threadParams[S4])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[%d] tied to S4 Frame Process\n", S4);
        exit(EXIT_FAILURE);
    }
    else {
        printf("Created thread[%d] tied to S4 Frame Process\n", S4);
    }

    ///< create frame write-back thread but perform no action because semaphore is not given
    rc = pthread_create(&threads[S5],                    // pointer to thread descriptor
                        &rt_sched_attr[S5],              // use specific attributes
                        S5_frame_writeback,             // thread function entry point
                        (void*)&(threadParams[S5])       // parameters to pass in
    );
    if (rc < EXIT_SUCCESS) {
        printf("Failed to create thread[%d] tied to S5 Frame Write-Back\n", S5);
        exit(EXIT_FAILURE);
    }
    else {
        printf("Created thread[%d] tied to S5 Frame Write-Back\n", S5);
    }

    ///< Create Sequencer thread, which like a cyclic executive, is highest priority
    printf("Starting S0 Sequencer...\n");
    sequencePeriods = S0_PERIODS;

    ///< Set up to signal SIGALRM if timer expires
    timer_create(MY_CLOCK, NULL, &timer_1);
    signal(SIGALRM, (void(*)()) S0_sequencer);

    ///< Arm the interval timer
    itime.it_interval.tv_sec = 0;
    itime.it_interval.tv_nsec = (1.0/S0_FREQ)*(NANOSEC_PER_SEC);
    itime.it_value.tv_sec = 0;
    itime.it_value.tv_nsec = (1.0/S0_FREQ)*(NANOSEC_PER_SEC);

    timer_settime(timer_1, flags, &itime, &last_itime);

    ///< Join back all threads
    for (i = 0; i < NUM_OF_THREADS; i++) {
        rc = pthread_join(threads[i], NULL);
        
        if (rc < EXIT_SUCCESS) {
            perror("main pthread_join");
        }
        else {
            printf("Joined thread[%d]\n", i);
        }
    }

    struct timespec frame_time;
    int j, newi;
    unsigned char* pptr = bigbuffer_read;
    unsigned int diff;
    printf("\n");
    printf("framecnt_read = %d\n", framecnt_read);
    printf("framecnt_diff_threshold = %d\n", framecnt_diff_threshold);
    printf("framecnt_select = %d\n", framecnt_select);
    printf("\n");

    ///< Test loop for S2_difference_threshold
    //pptr = bigbuffer_read
    //for (i = 0; i < framecnt_read; i++) {
    //    //printf("bigbuffer_read[%d] = %d\n", i, bigbuffer_diff_threshold[i]);
    //    //diff = (unsigned int)pptr[i * PHOTO_RES] - (unsigned int)pptr[(i + 1) * PHOTO_RES];
    //    diff = 0;
    //    for (j = 0; j < PHOTO_RES; j++) {
    //        diff += (unsigned int)pptr[i*PHOTO_RES + j] - (unsigned int)pptr[(i + 1)*PHOTO_RES + j];
    //    }
    //    syslog(LOG_INFO, "FinalProject (S2_frame_difference_threshold):  bigbuffer_read[%d] - bigbuffer_read[%d + 1] = %u", i, i, diff);
    //}

    printf("\n");

    ///< Test loop for S1_frame_acquisition
    //pptr = bigbuffer_read;
    //for (j = 0; j < framecnt_read; j++) {
    //    for (i = 0, newi = 0; i < size_buf[j]; i = i + 4, newi = newi + 2)
    //    {
    //        // Y1=first byte and Y2=third byte
    //        bigbuffer[newi] = pptr[(j * PHOTO_RES) + i];
    //        bigbuffer[newi + 1] = pptr[(j * PHOTO_RES) + i + 2];
    //    }
    //
    //    if (framecnt_read > -1)
    //    {
    //        clock_gettime(MY_CLOCK, &frame_time);
    //        dump_pgm(bigbuffer, (size_buf[j] / 2), j, &frame_time);
    //        //dump_pgm(bigbuffer, (size / 2), framecnt, &frame_time);
    //
    //        //printf("Dump YUYV converted to YY size %d\n", size);
    //        syslog(LOG_INFO, "FinalProject (S4_frame_process):               Read dump %j YUYV (%d) converted to YY (%d)\n", size_buf[j], size_buf[j]/2);
    //    }
    //}

    ///< Test loop for S3_frame_select
    pptr = bigbuffer_select;
    for (j = 0; j < framecnt_select; j++) {
        for (i = 0, newi = 0; i < size_buf_select[j]; i = i + 4, newi = newi + 2)
        {
            // Y1=first byte and Y2=third byte
            bigbuffer[newi] = pptr[(j * PHOTO_RES) + i];
            bigbuffer[newi + 1] = pptr[(j * PHOTO_RES) + i + 2];
        }
    
        if (framecnt_select > -1)
        {
            clock_gettime(MY_CLOCK, &frame_time);
            dump_pgm(bigbuffer, (size_buf_select[j] / 2), j, &frame_time);
            //dump_pgm(bigbuffer, (size / 2), framecnt, &frame_time);
    
            //printf("Dump YUYV converted to YY size %d\n", size);
            syslog(LOG_INFO, "FinalProject (S4_frame_process):               Select dump %j YUYV (%d) converted to YY (%d)\n", size_buf_select[j], size_buf_select[j]/2);
        }
    }

    fflush(stderr);

    //fprintf(stderr, ".");
    //syslog(LOG_ERR, ".");

    fflush(stdout);

    // service loop frame read
    // mainloop();

    /*************************************************************************
     * Insert my code above
     *************************************************************************/

     // shutdown of frame acquisition service
    stop_capturing();
    uninit_device();
    close_device();

    //fprintf(stderr, "\n");

    //printf("Ending Synchronome Project... writing syslog trace to ./syslog_trace_%02dmin.txt\n\n", S0_RUN_TIME_MIN);
    printf("Ending Synchronome Project... review syslog trace\n\n", S0_RUN_TIME_MIN);


    ///< Calculate number of lines to tail from syslog in relation to S0_RUN_TIME_MIN
    //sprintf(sys_buffer, "tail -%d /var/log/syslog | grep -n FinalProject > ./syslog_trace_%02dmin.txt", (int)((115.0/96.0)*S0_PERIODS + 13), S0_RUN_TIME_MIN);
    //sprintf(sys_buffer, "tail -%d /var/log/syslog | grep -n FinalProject > ./syslog_trace_%02dmin.txt", 75000, S0_RUN_TIME_MIN);

    ///< Delay for 1 sec before checking syslog trace
    //usleep(1000000);

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
