/////
//
// Copyright 2019 Martin Peck <coderman@protonmail.com>
// this work is derived from:
//
//  * rngd.c -- Random Number Generator daemon
//  Copyright (C) 2001 Philipp Rumpf <prumpf@mandrakesoft.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
/////

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <argp.h>
#include <ctype.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/types.h>
#include <linux/random.h>
#include <fcntl.h>
#include <errno.h>
#include <iostream>
#include <string>
#include <list>

#include "fips.h"

using namespace std;

// MUST be same as FIPS buffer size expectation: 20,000 bits
#define RND_BLOCK_SIZE 2500

typedef unsigned char       byte;
typedef unsigned long       u32;
typedef unsigned long long  u64;

static volatile bool  terminate_s = false;

void
shutdown ()
{
    terminate_s = true;
    // other options...
    // kill (getpid(), 9);
    // _exit (0);
}

string
tostr (u32  val)
{
    char buf[16];
    snprintf (buf, sizeof(buf), "%lu", val);
    return ((string)buf);
}

string
tostr (int  val)
{
    char buf[16];
    snprintf (buf, sizeof(buf), "%d", val);
    return ((string)buf);
}

string
tostr (long  val)
{
    char buf[16];
    snprintf (buf, sizeof(buf), "%ld", val);
    return ((string)buf);
}

string
tostr (float val)
{
    char buf[32];
    snprintf (buf, sizeof(buf), "%f", val);
    return ((string)buf);
}


class Mutex
{
  public:
    Mutex ();
    ~Mutex ();

    void  acquire (void);
    bool  tryAcquire (void);
    void  release (void);
  private:
    pthread_mutex_t  mutex_;
};

Mutex::Mutex ()
{
    pthread_mutex_init (&mutex_, 0);
}

Mutex::~Mutex ()
{
    pthread_mutex_destroy (&mutex_);
}

void
Mutex::acquire ()
{
    pthread_mutex_lock (&mutex_);
}

bool
Mutex::tryAcquire ()
{
    int result;
    result = pthread_mutex_trylock (&mutex_);
    if (result < 0) {
        return (false);
    }
    return (true);
}

void
Mutex::release ()
{
    pthread_mutex_unlock (&mutex_);
}



class MutexLock
{
  public:
    MutexLock (Mutex & mutex);
    ~MutexLock ();
  private:
    Mutex *  mutex_;
};

MutexLock::MutexLock (Mutex & mutex)
{
    mutex_ = &mutex;
    mutex_->acquire ();
}

MutexLock::~MutexLock ()
{
    mutex_->release ();
}


class Timer
{
  public:
    Timer ();
    void  start (void);
    void  stop (void);
    u64  elapsed (void) const;

  private:
    struct timeval  start_;
    struct timeval  end_;
    u64       diff_;
};

Timer::Timer ()
{
    memset (&start_, 0, sizeof(start_));
    memset (&end_, 0, sizeof(end_));
    diff_ = 0;
}

void
Timer::start ()
{
    gettimeofday (&start_, 0);
}

void
Timer::stop ()
{
    gettimeofday (&end_, 0);
    diff_ = (end_.tv_sec - start_.tv_sec) * 1000000L;
    if (end_.tv_usec > start_.tv_usec) {
        diff_ += end_.tv_usec - start_.tv_usec;
    }
    else {
        diff_ -= 1000000L;
        diff_ += (1000000L - start_.tv_usec) + end_.tv_usec;
    }
}

u64
Timer::elapsed () const
{
    return (diff_);
}


class Stats
{
  public:
    Stats ();
    void  reset (void);
    void  add (const Timer &  timer);
    void  result (u64 &  min,
                  u64 &  avg,
                  u64 &  max,
                  u64 &  total);

  private:
    u32  count_;
    u64  min_;
    u64  max_;
    u64  total_;
};

Stats::Stats ()
{
    reset ();
}

void
Stats::reset ()
{
    count_ = min_ = max_ = total_ = 0L;
}

void
Stats::add (const Timer &  timer)
{
    u64  cval = timer.elapsed ();
    count_++;
    total_ += cval;
    if (min_ == 0 || cval < min_) min_ = cval;
    if (max_ == 0 || cval > max_) max_ = cval;
}

void
Stats::result (u64 &  min,
               u64 &  avg,
               u64 &  max,
               u64 &  total)
{
    if (count_ == 0) {
        min = avg = max = total = 0;
        return;
    }

    min   = min_;
    avg   = total_ ? total_ / count_ : 0;
    max   = max_;
    total = total_;
}


class Log
{
  public:
    Log () : fd_(-1) {}
    ~Log ();
    bool  open (const string &  filename);
    bool  open (int fd);
    void  write (const string &  msg);
    static string  timestamp (void);

  private:
    int    fd_;
    Mutex  lock_;
};

Log::~Log ()
{
    if (fd_ >= 0) {
        close (fd_);
        fd_ = -1;
    }
}

bool
Log::open (const string &  filename)
{
    int  flags = O_WRONLY;
    int  mode  = S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH;
    struct stat  filestat;

    MutexLock  lock(lock_);
    if (fd_ >= 0) {
        close (fd_);
        fd_ = -1;
    }

    if (stat (filename.c_str(), &filestat) != 0) {
        flags |= O_CREAT;
    }
    else {
        flags |= O_APPEND;
    }

    fd_ = ::open (filename.c_str(), flags, mode);
    if (fd_ < 0) 
        return (false);

    return (true);
}

bool
Log::open (int fd)
{
    MutexLock  lock(lock_);
    if (fd_ >= 0) {
        close (fd_);
        fd_ = -1;
    }
    fd_ = dup (fd);
    if (fd_ < 0)
        return (false);

    return (true);
}

void
Log::write (const string &  msg)
{
    MutexLock  lock(lock_);
    if (fd_ < 0)
        return;
    string  currtime = timestamp();
    ::write (fd_, currtime.c_str(), currtime.size());
    ::write (fd_, msg.c_str(), msg.size());
    ::write (fd_, "\n", 1);
    fsync(fd_);
}

string
Log::timestamp ()
{
    const int       buffMax = 128;
    char            tmpBuff[buffMax];
    struct tm       today;
    time_t          now;
    struct timeval  hrTime;
    const char * dayArray[] = { "Sun", "Mon", "Tue", "Wed", "Thr", "Fri", "Sat" };
    const char * monthArray[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    gettimeofday (&hrTime, 0);
    now = time(0);
    localtime_r (&now, &today);

    snprintf (tmpBuff, buffMax, "[%s %s %2.2d %2.2d:%2.2d:%2.2d-%6.6lu] ",
              dayArray[(today.tm_wday)], monthArray[(today.tm_mon)],
              today.tm_mday, today.tm_hour, today.tm_min, today.tm_sec, hrTime.tv_usec);

    return ((string)tmpBuff);
}

// global application log handle.  If this cannot be initialized
// application will immediately exit.
//
static Log *  log_s = 0;


class LogQueue
{
  public:
    LogQueue ();
    ~LogQueue ();
    bool  initialize (const string &  logdir,
                      const string &  prefix,
                      u32             filemax);
    bool  currLog (Log *&  log);
    bool  newLog (Log *&  log);
    bool  closeAll (void);

    typedef list<string>  t_FileList;

  private:
    string      logdir_;
    string      prefix_;
    Log *       log_;
    u32         filemax_;
    t_FileList  files_;

    string  createFilePath (void);
};

LogQueue::LogQueue ()
{
    log_ = 0;
}

LogQueue::~LogQueue ()
{
    if (log_)
        delete log_;
}

bool
LogQueue::initialize (const string &  logdir,
                      const string &  prefix,
                      u32             filemax)
{
    logdir_ = logdir;
    if (*(logdir_.c_str() + logdir_.size() -1) != '/') {
        logdir_ += (string)"/";
    }
    prefix_  = prefix;
    filemax_ = filemax;
    return (true);
}

bool  
LogQueue::currLog (Log *&  log)
{
    if (!log_)
        return (newLog (log));
    log = log_;
    return (true);
}

bool  
LogQueue::newLog (Log *&  log)
{
    string path = createFilePath ();
    if (log_)
        delete log_;
    log_ = new Log;
    if (log_->open (path) == false) {
        delete log_;
        log_ = 0;
        return (false);
    }
    if (filemax_ > 0) {
        files_.push_front (path);
        if (files_.size () > filemax_) {
            path = files_.back ();
            files_.pop_back ();
            unlink (path.c_str());
        }
    }
    log = log_;
    return (true);
}

bool  
LogQueue::closeAll ()
{
    if (log_) {
        delete log_;
        log_ = 0;
    }
    return (true);
}

string  
LogQueue::createFilePath ()
{
    const int  max = 256;
    char buf[max];
    snprintf (buf, max, "%s-%d.log", prefix_.c_str(), time(0));
    return (logdir_ + (string)buf);
}


typedef struct s_rng_stats {
    u64    bad_fips_blocks;
    u64    fips_monobit;
    u64    fips_poker;
    u64    fips_runs;
    u64    fips_longruns;
    u64    fips_contruns;
    u64    good_fips_blocks;
    u64    total_hwrng_bytes;
    u64    total_entadd_bytes;
    u64    total_devrnd_writeable;
    Stats  hwrng_block_stats;
    Stats  hwrng_fips_stats;
    Stats  random_starve_stats;
} t_rng_stats;

static t_rng_stats  curr_rng_stats_s = { 0 };
static t_rng_stats  all_rng_stats_s  = { 0 };

string
prepareStatDesc (t_rng_stats *  stats)
{
    string       desc;
    const int    len = 1024;
    char *       buf = new char[len];
    u64          min, avg, max, total;

    snprintf (buf, len,
        " bad fips blocks ......: %llu\n"
        "   monobit failures ___: %llu\n"
        "   poker run failures _: %llu\n"
        "   bit run failures  __: %llu\n"
        "   long run failures __: %llu\n"
        "   cont run failures __: %llu\n"
        " good fips blocks .....: %llu\n"
        " hwrng read bytes .....: %llu\n"
        " entropy add bytes ....: %llu\n"
        " random writeable cnt .: %llu\n",
        stats->bad_fips_blocks,
        stats->fips_monobit,
        stats->fips_poker,
        stats->fips_runs,
        stats->fips_longruns,
        stats->fips_contruns,
        stats->good_fips_blocks,
        stats->total_hwrng_bytes,
        stats->total_entadd_bytes,
        stats->total_devrnd_writeable);
    desc = (string)buf;

    stats->hwrng_block_stats.result (min, avg, max, total);
    snprintf (buf, len, " hw entropy read stats ....: \tmin: %llu\tavg:  %llu\tmax: %llu\ttotal: %llu\n", min, avg, max, total);
    desc = desc + (string)buf;

    stats->hwrng_fips_stats.result (min, avg, max, total);
    snprintf (buf, len, " rng fips check stats .....: \tmin: %llu\tavg:  %llu\tmax: %llu\ttotal: %llu\n", min, avg, max, total);
    desc = desc + (string)buf;

    stats->random_starve_stats.result (min, avg, max, total);
    snprintf (buf, len, " random recv starve stats .: \tmin: %llu\tavg:  %llu\tmax: %llu\ttotal: %llu\n", min, avg, max, total);
    desc = desc + (string)buf;

    delete [] buf;
    return (desc);
}

void
writeStats (Log *  log) {
    string       msg;

    t_rng_stats  currstats = curr_rng_stats_s;
    memset (&curr_rng_stats_s, 0, sizeof(curr_rng_stats_s));
    t_rng_stats  allstats  = all_rng_stats_s;

    msg = msg + (string)"\n--- current mtrngd stats:\n";
    msg = msg + prepareStatDesc (&currstats);


    msg = msg + (string)"\n\n--- TOTAL mtrngd stats:\n";
    msg = msg + prepareStatDesc (&allstats);

    log->write (msg);

    return;
}

void
dumpStatus (const string &  logfile) {
    Log * log = new Log;
    struct stat  st;
    if (!stat (logfile.c_str(), &st)) {
        unlink (logfile.c_str());
    }
    if (log->open (logfile) == true) {
        string  msg;
        t_rng_stats  allstats  = all_rng_stats_s;
        msg = "Current MTRNGD Status:\n";
        msg = msg + prepareStatDesc (&allstats);
        log->write (msg);
    }
    delete log;
}

void
setFillSigMask () {
    int how = SIG_SETMASK;
    sigset_t  signals;
    sigfillset (&signals);
    pthread_sigmask (how, &signals, 0);
    sigfillset (&signals);
    sigprocmask (how, &signals, 0);
}

bool
handleSignal (int num) {
    switch (num) {
        case SIGQUIT: terminate_s = true; break;
        case SIGTERM: terminate_s = true; break;

      default:
        break;
    };

    return (terminate_s);
}

void
processSignals (const string &  logfile) {
    bool done = false;
    int result;
    int sigNumber;
    sigset_t     signals;
    sigfillset (&signals);
    while (!done) {
        result = sigwait (&signals, &sigNumber);
        if (result == 0) {
            if (sigNumber == SIGUSR1) { dumpStatus (logfile); }
            if (handleSignal (sigNumber) == true) {
                return;
            }
        }
    }
}


class LockedMem
{
  public:
    LockedMem ();
    ~LockedMem ();
    bool    allocate (u32  size);
    byte *  buffer(void);  
    u32     length(void);
    u32     datalen (void);
    void    datalen (u32 datalen);

  private:
    byte *  buf_;
    u32     len_;
    u32     datalen_;
    LockedMem (const LockedMem & copy);
    LockedMem & operator = (const LockedMem & copy);
};

LockedMem::LockedMem ()
{
    buf_ = 0;
    len_ = datalen_ = 0;
}

LockedMem::~LockedMem ()
{
    if (buf_) {
        munlock (buf_, len_);
        delete [] buf_;
        buf_ = 0;
    }
}

bool    
LockedMem::allocate (u32  size)
{
    if (buf_) {
        delete [] buf_;
    }
    datalen_ = 0;
    buf_ = new byte[size];
    len_ = size;
    if (mlock (buf_, len_) != 0) {
        delete [] buf_;
        buf_ = 0;
        len_ = 0;
        return (false);
    }
    return (true);
}

byte *  
LockedMem::buffer()
{
    return (buf_);
}

u32     
LockedMem::length()
{
    return (len_);
}
u32     
LockedMem::datalen ()
{
    return (datalen_);
}

void    
LockedMem::datalen (u32 datalen)
{
    datalen_ = datalen;
}



class RandXfer
{
  public:
    static bool  initialize (u32  blockSize);
    static bool  getDestMem (LockedMem *& mem);
    static bool  getRandMem (LockedMem *& mem);
    static void  destroy (void);

  private:
    static bool         isActive_s;
    static u32          blockSize_s;
    static LockedMem *  currDestMem_s;
    static LockedMem *  currRandMem_s;
    static LockedMem *  currSpareMem_s;
    static Mutex        dataLock_s;
    static Mutex        waitLock_s;
    static bool         isWaiting_s;
    static Mutex        blockLock_s;
};

bool         RandXfer::isActive_s      = false;
u32          RandXfer::blockSize_s     = 0;
LockedMem *  RandXfer::currDestMem_s   = 0;
LockedMem *  RandXfer::currRandMem_s   = 0;
LockedMem *  RandXfer::currSpareMem_s  = 0;
Mutex        RandXfer::dataLock_s;
Mutex        RandXfer::waitLock_s;
bool         RandXfer::isWaiting_s      = false;
Mutex        RandXfer::blockLock_s;

bool  
RandXfer::initialize (u32  blockSize)
{
    MutexLock  lock(dataLock_s);

    if (isActive_s) return (false);

    blockSize_s = blockSize;
    currDestMem_s = new LockedMem();
    currRandMem_s = new LockedMem();
    currSpareMem_s = new LockedMem();
    
    if ( (currDestMem_s->allocate (blockSize) == false) ||
         (currRandMem_s->allocate (blockSize) == false) ||
         (currSpareMem_s->allocate (blockSize) == false)   ) {
        log_s->write ((string)"Unable to allocate memory buffers in call to RandXfer::initialize.");
        return (false);
    }
    isActive_s = true;
    isWaiting_s = false;

    log_s->write ((string)"Random xfer memory buffers allocated and locked.");

    return (true);
}

bool  
RandXfer::getDestMem (LockedMem *& mem)
{
    mem = 0;
    {
        MutexLock lock(dataLock_s);
        if (isActive_s == false) return (false);
        if (currDestMem_s->datalen() < currDestMem_s->length()) {
            mem = currDestMem_s;
        }
        else if (currSpareMem_s->datalen() < currSpareMem_s->length()) {
            mem = currSpareMem_s;
            currSpareMem_s = currDestMem_s;
            currDestMem_s = mem;
        }
        else { // prepare to wait for other thread to pass us an empty buffer
            waitLock_s.acquire ();
        }
        // if we have a buffer, check to see if we need to unblock the entropy writer
        if (mem) {
            waitLock_s.acquire ();
            if (isWaiting_s) {
                isWaiting_s = false;
                blockLock_s.release ();
            }
            waitLock_s.release ();
        }
    }
    if (mem == 0) {
        // now outside dataLock scope, perform blocking wait
        isWaiting_s = true;
        blockLock_s.acquire ();
        waitLock_s.release ();
        blockLock_s.acquire ();
        blockLock_s.release ();

        // grab new empty record 
        {
            MutexLock lock(dataLock_s);
            mem = currSpareMem_s;
            currSpareMem_s = currDestMem_s;
            currDestMem_s = mem;
        }
    }

    return (true);
}

bool  
RandXfer::getRandMem (LockedMem *& mem)
{
    mem = 0;
    {
        MutexLock lock(dataLock_s);
        if (isActive_s == false) return (false);
        if (currSpareMem_s->datalen() > 0) {
            mem = currSpareMem_s;
            currSpareMem_s = currRandMem_s;
            currRandMem_s = mem;
        }
        else { // prepare to wait for other thread to pass us a buffer of random
            waitLock_s.acquire ();
        }
        if (mem) {
            waitLock_s.acquire ();
            if (isWaiting_s) {
                isWaiting_s = false;
                blockLock_s.release ();
            }
            waitLock_s.release ();
        }
    }
    if (mem == 0) {
        // now outside dataLock scope, perform blocking wait
        isWaiting_s = true;
        blockLock_s.acquire ();
        waitLock_s.release ();
        blockLock_s.acquire ();
        blockLock_s.release ();

        // grab new empty record 
        {
            MutexLock lock(dataLock_s);
            mem = currSpareMem_s;
            currSpareMem_s = currRandMem_s;
            currRandMem_s = mem;
        }
    }

    return (true);
}

void  
RandXfer::destroy ()
{
    MutexLock  lock(dataLock_s);
    isActive_s = false;

    // Don't delete memory, as it may be in use.  Let host
    // clean up when process exits.
    return;
}


enum {
    /* MRP register layout
     * 31:22 reserved
     * 21:16 string filter count
     * 15:15 string filter failed
     * 14:14 string filter enabled
     * 13:13 raw bits enabled
     * 12:10 dc bias value
     * 09:08 noise source select
     * 07:07 reserved
     * 06:06 rng enabled
     * 05:05 reserved
     * 04:00 current byte count
    */
    MSR_VIA_RNG             = 0x110b,
    VIA_STRFILT_CNT_SHIFT   = 16,
    VIA_STRFILT_FAIL        = (1 << 15),
    VIA_STRFILT_ENABLE      = (1 << 14),
    VIA_STRFILT_MIN         = 8,
    VIA_STRFILT_MAX         = 63,
    VIA_STRFILT_MASK        = (VIA_STRFILT_MAX << VIA_STRFILT_CNT_SHIFT),
    VIA_RAWBITS_ENABLE      = (1 << 13),
    VIA_NOISE_SRC_SHIFT     = 8,
    VIA_NOISE_SRC_MASK      = (3 << VIA_NOISE_SRC_SHIFT),
    VIA_RNG_ENABLE          = (1 << 6),
    VIA_DCBIAS_SHIFT        = 10,
    VIA_DCBIAS_MAX          = 7,
    VIA_DCBIAS_MASK         = (VIA_DCBIAS_MAX << VIA_DCBIAS_SHIFT),
    VIA_XSTORE_CNT_MASK     = 0x0F,
    VIA_RNG_CHUNK_8         = 0x00, /* 64 rand bits, 64 stored bits */
};

class ViaMsrDevice;

class ViaMsrRngConfig
{
  public:
    ViaMsrRngConfig () {}
    ViaMsrRngConfig (u32  lomsr) : lomsr_(lomsr) {}
    bool  getRngEnable (void);
    void  setRngEnable (bool  enabled);
    u32   getDcBias (void);
    void  setDcBias (u32  bias);
    bool  getStrfltEnable (void);
    void  setStrfltEnable (bool  enabled);
    u32   getStrfltLength (void);
    void  setStrfltLength (u32  length);
    bool  getStrfltFault (void);
    void  setStrfltFault (bool  faulted);
    u32   getNoiseSrc (void);
    void  setNoiseSrc (u32  source);
    bool  getRawbitEnable (void);
    void  setRawbitEnable (bool  enabled);
    string  asString (void);

    bool operator == (const ViaMsrRngConfig & b) {
        if ( ((lomsr_ & VIA_RNG_ENABLE)      == (b.lomsr_ & VIA_RNG_ENABLE))     &&
             ((lomsr_ & VIA_STRFILT_ENABLE)  == (b.lomsr_ & VIA_STRFILT_ENABLE)) &&
             ((lomsr_ & VIA_STRFILT_MASK)    == (b.lomsr_ & VIA_STRFILT_MASK))   &&
             ((lomsr_ & VIA_DCBIAS_MASK)     == (b.lomsr_ & VIA_DCBIAS_MASK))    &&
             ((lomsr_ & VIA_RAWBITS_ENABLE)  == (b.lomsr_ & VIA_RAWBITS_ENABLE)) &&
             ((lomsr_ & VIA_NOISE_SRC_MASK)  == (b.lomsr_ & VIA_NOISE_SRC_MASK))   ) {
            return (true);
        }
        return (false);
    }
    bool operator != (const ViaMsrRngConfig & b) { (*this == b) ? false : true; }

  private:
    u32  lomsr_;
    u32  applyConfig (u32  srcMsrBits);
    friend class ViaMsrDevice;
};

bool  
ViaMsrRngConfig::getRngEnable ()
{
    return ((lomsr_ & VIA_RNG_ENABLE) ? true : false);
}

void  
ViaMsrRngConfig::setRngEnable (bool  enabled)
{
    if (enabled) 
        lomsr_ |= VIA_RNG_ENABLE;
    else
        lomsr_ &= ~(VIA_RNG_ENABLE);
}

u32   
ViaMsrRngConfig::getDcBias ()
{
    return ((lomsr_ & VIA_DCBIAS_MASK) >> VIA_DCBIAS_SHIFT);
}

void  
ViaMsrRngConfig::setDcBias (u32  bias)
{
    lomsr_ |= VIA_DCBIAS_MASK & (bias << VIA_DCBIAS_SHIFT);
}

bool  
ViaMsrRngConfig::getStrfltEnable ()
{
    return ((lomsr_ & VIA_STRFILT_ENABLE) ? true : false);
}

void  
ViaMsrRngConfig::setStrfltEnable (bool  enabled)
{
    if (enabled) 
        lomsr_ |= VIA_STRFILT_ENABLE;
    else
        lomsr_ &= ~(VIA_STRFILT_ENABLE);
}

u32   
ViaMsrRngConfig::getStrfltLength ()
{
    return ((lomsr_ & VIA_STRFILT_MASK) >> VIA_STRFILT_CNT_SHIFT);
}

void  
ViaMsrRngConfig::setStrfltLength (u32  length)
{
    lomsr_ |= VIA_STRFILT_MASK & (length << VIA_STRFILT_CNT_SHIFT);
}

bool  
ViaMsrRngConfig::getStrfltFault ()
{
    return ((lomsr_ & VIA_STRFILT_FAIL) ? true : false);
}

void  
ViaMsrRngConfig::setStrfltFault (bool  faulted)
{
    if (faulted) 
        lomsr_ |= VIA_STRFILT_FAIL;
    else
        lomsr_ &= ~(VIA_STRFILT_FAIL);
}

u32   
ViaMsrRngConfig::getNoiseSrc ()
{
    return ((lomsr_ & VIA_NOISE_SRC_MASK) >> VIA_NOISE_SRC_SHIFT);
}

void  
ViaMsrRngConfig::setNoiseSrc (u32  source)
{
    lomsr_ |= VIA_NOISE_SRC_MASK & (source << VIA_NOISE_SRC_SHIFT);
}

bool  
ViaMsrRngConfig::getRawbitEnable ()
{
    return ((lomsr_ & VIA_RAWBITS_ENABLE) ? true : false);
}

void  
ViaMsrRngConfig::setRawbitEnable (bool  enabled)
{
    if (enabled) 
        lomsr_ |= VIA_RAWBITS_ENABLE;
    else
        lomsr_ &= ~(VIA_RAWBITS_ENABLE);
}

string  
ViaMsrRngConfig::asString ()
{
    char buf[256];
    const char *  rngsrc_disp = "both";
    if (((lomsr_ & VIA_NOISE_SRC_MASK) >> VIA_NOISE_SRC_SHIFT) == 0) {
        rngsrc_disp = "A";
    }
    else if (((lomsr_ & VIA_NOISE_SRC_MASK) >> VIA_NOISE_SRC_SHIFT) == 1) {
        rngsrc_disp = "B";
    }

    snprintf (buf, sizeof(buf), "rng enabled: %s, rawbit enabled: %s, strflt enabled: %s, strflt len: %d, strflt faulted: %s, rng source: %s, dc bias: %d",
        (lomsr_ & VIA_RNG_ENABLE) ? "true" : "false",
        (lomsr_ & VIA_RAWBITS_ENABLE) ? "true" : "false",
        (lomsr_ & VIA_STRFILT_ENABLE) ? "true" : "false",
        ((lomsr_ & VIA_STRFILT_MASK) >> VIA_STRFILT_CNT_SHIFT),
        (lomsr_ & VIA_STRFILT_FAIL) ? "yes" : "no",
        rngsrc_disp,
        ((lomsr_ & VIA_DCBIAS_MASK) >> VIA_DCBIAS_SHIFT));

    return ((string)buf);
}

u32  
ViaMsrRngConfig::applyConfig (u32  srcMsrBits)
{
    // first lets clear bits we will subsequently set
    srcMsrBits &= ~(VIA_RNG_ENABLE);
    srcMsrBits &= ~(VIA_STRFILT_ENABLE);
    srcMsrBits &= ~(VIA_STRFILT_MASK);
    srcMsrBits &= ~(VIA_STRFILT_FAIL);
    srcMsrBits &= ~(VIA_DCBIAS_MASK);
    srcMsrBits &= ~(VIA_RAWBITS_ENABLE);
    srcMsrBits &= ~(VIA_NOISE_SRC_MASK);
    // now apply bits based on current config
    srcMsrBits |= lomsr_ & VIA_RNG_ENABLE;
    srcMsrBits |= lomsr_ & VIA_STRFILT_ENABLE;
    srcMsrBits |= lomsr_ & VIA_STRFILT_MASK;
    srcMsrBits |= lomsr_ & VIA_STRFILT_FAIL;
    srcMsrBits |= lomsr_ & VIA_DCBIAS_MASK;
    srcMsrBits |= lomsr_ & VIA_RAWBITS_ENABLE;
    srcMsrBits |= lomsr_ & VIA_NOISE_SRC_MASK;

    return (srcMsrBits);
}


class ViaMsrDevice
{
  public:
    ViaMsrDevice (const string &  device);
    ~ViaMsrDevice ();

    bool  readViaMsr (ViaMsrRngConfig &  config);
    bool  writeViaMsr (ViaMsrRngConfig &  config);

  private:
    string  device_;
    int     fd_;
};

ViaMsrDevice::ViaMsrDevice (const string &  device)
{
    device_ = device;
    fd_     = open (device.c_str (), O_RDWR);
}

ViaMsrDevice::~ViaMsrDevice ()
{
    if (fd_ >= 0) {
        close (fd_);
        fd_ = -1;
    }
}

bool  
ViaMsrDevice::readViaMsr (ViaMsrRngConfig &  config)
{
    if (fd_ < 0) 
        return (false);

    u32  data[2];
    if (pread (fd_, data, 8, MSR_VIA_RNG) != 8) {
        return (false);
    }
    config.lomsr_ = data[0];

    return (true);
}

bool  
ViaMsrDevice::writeViaMsr (ViaMsrRngConfig &  config)
{
    if (fd_ < 0) 
        return (false);

    u32  data[2];
    // read first to initialize MSR values and use config to apply relevant bits
    if (pread (fd_, data, 8, MSR_VIA_RNG) != 8) {
        return (false);
    }
    data[0] = config.applyConfig (data[0]);
    if (pwrite (fd_, data, 8, MSR_VIA_RNG) != 8) {
        return (false);
    }
    return (true);
}


class Entropy
{
  public:
    virtual ~Entropy () {};
    virtual u32     blockSize (void) = 0;
    virtual byte *  readBlock (void) = 0;
    virtual u32     readU32 (void) = 0;
};


class XstoreRng : public Entropy
{
  public:
    XstoreRng (u32  blockSize);
    XstoreRng (ViaMsrRngConfig * config,
               ViaMsrDevice *    msr,
               u32               blockSize);
    virtual ~XstoreRng ();
    virtual u32     blockSize (void) { return (blockSize_); }
    virtual byte *  readBlock (void);
    virtual u32     readU32 (void);
                                                                                                                                                                          
  private:
    u32                blockSize_;
    byte *             buff_;
    ViaMsrRngConfig *  config_;
    ViaMsrDevice *     msr_;
    // the destination storage must be static (text segment) for xstore
    static u64         dest_;
};

u64 XstoreRng::dest_;

XstoreRng::XstoreRng (u32  blockSize)
{
    blockSize_ = blockSize;
    buff_      = new byte[blockSize_];
    config_    = 0;
    msr_       = 0;
}

XstoreRng::XstoreRng (ViaMsrRngConfig * config,
                      ViaMsrDevice *    msr,
                      u32               blockSize)
{
    blockSize_ = blockSize;
    buff_      = new byte[blockSize_];
    config_    = config;
    msr_       = msr;
}

XstoreRng::~XstoreRng ()
{
    if (buff_) {
        delete [] buff_;
        buff_ = 0;
    }
}

byte *
XstoreRng::readBlock ()
{
    byte *  curr = buff_;
    u32     remaining = blockSize_;
    u32     edx_in;
    u32     eax_out;

    while (remaining > 0) {
        edx_in = VIA_RNG_CHUNK_8;
        asm(".byte 0x0F,0xA7,0xC0 /* xstore %%edi (addr=%0) */"
                :"=m"(dest_), "=a"(eax_out)
                :"D"(&dest_), "d"(edx_in));

        // if we are using the MSR to configure the RNG, make sure that the
        // current configuration matches what we started with, otherwise
        // force a reconfigure...
        //
        if (msr_) {
            ViaMsrRngConfig  currconfig (eax_out);
            if (currconfig != *config_) {
                log_s->write ((string) "WARNING: current rng configuration: " + currconfig.asString() +
                              " does not match desired configuration: " + config_->asString() +
                              " in call to XstoreRng::readBlock; reconfiguring via MSR.");
                if (msr_->writeViaMsr (*config_) == false) {
                    log_s->write ((string) "Fatal error trying to write RNG config via MSR.  Exiting.");
                    return (0);
                }
                continue;
            }
        }

        int rlen = eax_out & VIA_XSTORE_CNT_MASK;
        if (rlen != 8) { 
            continue;
        }
        if (remaining >= 8) {
            memcpy (curr, &dest_, sizeof (dest_));
            curr      += rlen;
            remaining -= rlen;
        }
        else {
            while (remaining) {
                *(curr++) = (byte)dest_;
                dest_ >>= 8;
                remaining--;
            }
        }
        // NOTE: as of 2.6.8.1 longhaul CPU freq scaling will cause segfaults here
        // unless a short sleep is performed between every iteration of the xstore
        // loop.  does not happen in kernel space xstore via hw_random.c ?
    }
    return (buff_);
}

u32
XstoreRng::readU32 ()
{
    static  u64  cdest;
    u32     result = 0;
    u32     edx_in;
    u32     eax_out;

    while (result == 0) {
        edx_in = VIA_RNG_CHUNK_8;
        asm(".byte 0x0F,0xA7,0xC0 /* xstore %%edi (addr=%0) */"
                :"=m"(cdest), "=a"(eax_out)
                :"D"(&cdest), "d"(edx_in));

        int rlen = eax_out & VIA_XSTORE_CNT_MASK;
        if (rlen < sizeof(result)) { 
	    result = 0;
            continue;
        }
        memcpy (&result, &cdest, sizeof (result));
    }
    return (result);
}


class DevRng : public Entropy
{
  public:
    DevRng (const string &  device,
            u32             blockSize);
    virtual ~DevRng ();
    virtual u32     blockSize (void) { return (blockSize_); }
    virtual byte *  readBlock (void);
    virtual u32     readU32 (void);

  private:
    u32     blockSize_;
    int     rngFd_;
    byte *  buff_;
};

DevRng::DevRng (const string &  device,
                u32             blockSize)
{
    blockSize_ = blockSize;
    rngFd_     = open (device.c_str(), O_RDONLY);
    if (rngFd_ < 0) {
        buff_ = 0;
    }
    else {
        buff_ = new byte[blockSize_];
    }
}

DevRng::~DevRng ()
{
    if (buff_) {
        delete [] buff_;
        buff_ = 0;
    }
    if (rngFd_ >= 0) {
        close (rngFd_);
        rngFd_ = -1;
    }
}

byte *
DevRng::readBlock ()
{
    if (rngFd_ < 0) {
        log_s->write ("Unable to open random device.");
        shutdown ();
        return (0);
    }

    byte * curr = buff_;
    u32  remaining = blockSize_;
    while (remaining > 0) {
        int  rlen = read (rngFd_, curr, remaining);
        if (rlen < 0) {
            return (0);
        }
	curr      += rlen;
        remaining -= rlen;
    }
    return (buff_);
}

u32
DevRng::readU32 ()
{
    if (rngFd_ < 0) {
        return (0);
    }

    u32  result;
    byte *  curr = (byte *)&result;
    u32  remaining = sizeof(result);

    while (remaining > 0) {
        int  rlen = read (rngFd_, curr, remaining);
        if (rlen < 0) {
            return (0);
        }
	curr      += rlen;
        remaining -= rlen;
    }
    return (result);
}


class SysThread
{
  public:
    SysThread ();
    virtual ~SysThread () {};
    virtual void threadMain (void) = 0;
    bool run (void);

  private:
    pthread_t       threadId_;
    bool            running_;
    bool            created_;

    static void * redirector (void *);
};

SysThread::SysThread () {
    running_ = false;
    created_ = false;
}

bool
SysThread::run ()
{
    if (created_ == true) {
        return (false);
    }
    int retVal;
    pthread_attr_t threadAttr;
    pthread_attr_init(&threadAttr);

    retVal = pthread_create(&threadId_, &threadAttr, &redirector, this);
    if (retVal < 0) {
        return (false);
    }

    retVal = pthread_detach(threadId_);
    if (retVal < 0) {
      return (false);
    }
    created_ = true;
    running_ = true;

    return (true);
}

void *
SysThread::redirector (void * arg)
{
    SysThread *  threadPtr;
    threadPtr = reinterpret_cast<SysThread *>(arg);
    threadPtr->threadMain ();
    threadPtr->running_ = false;

    return (0);
}

class HwEntropyThread : public SysThread
{
  public:
    HwEntropyThread (Entropy *  entropySource, bool relaxed_fips ) { entropySrc_ = entropySource; relaxed_fips_ = relaxed_fips; }
    virtual ~HwEntropyThread () { if (entropySrc_) delete entropySrc_; }
    virtual void threadMain (void);
  private:
    Entropy *  entropySrc_;
    bool  relaxed_fips_;
};

#include "fips.c"

void
HwEntropyThread::threadMain () {
    int          result;
    u32          remaining;
    u32          last32 = 0;
    bool         status;
    bool         fipsPassed;
    bool         fips_hard_fail;
    byte *       data;
    byte *       dest;
    LockedMem *  mem;
    Timer        watch;
    struct fips_ctx  fipsctx;


    int  rndbSize = entropySrc_->blockSize ();

    log_s->write ((string)"HwEntropyThread::threadMain invoked.");

    last32 = entropySrc_->readU32 ();
    fips_init(&fipsctx, last32);

    while (terminate_s == false) {
        if (RandXfer::getDestMem (mem) == false) {
            log_s->write ((string)"Fatal error trying to get RandXfer dest memory.  Exiting."); 
            shutdown ();
            return;
        }

        fipsPassed = false;
        while (!fipsPassed) {
            dest = mem->buffer();
            watch.start ();
            data = entropySrc_->readBlock ();
            watch.stop ();
            all_rng_stats_s.hwrng_block_stats.add (watch);
            curr_rng_stats_s.hwrng_block_stats.add (watch);
            if (data == 0) {
                log_s->write ((string)"Fatal error trying to read entropy.  Exiting."); 
                shutdown ();
                return;
            }
            all_rng_stats_s.total_hwrng_bytes += rndbSize;
            curr_rng_stats_s.total_hwrng_bytes += rndbSize;
            memcpy (dest, data, rndbSize);
            mem->datalen(mem->length());
            watch.start ();
            result = fips_run_rng_test (&fipsctx, dest);
            watch.stop ();
            all_rng_stats_s.hwrng_fips_stats.add (watch);
            curr_rng_stats_s.hwrng_fips_stats.add (watch);
	    fips_hard_fail = false;
            if (result) {
                if (result & FIPS_RNG_MONOBIT) {
                    all_rng_stats_s.fips_monobit++;
                    curr_rng_stats_s.fips_monobit++;
	            fips_hard_fail = true;
                }
                if (result & FIPS_RNG_POKER) {
                    all_rng_stats_s.fips_poker++;
                    curr_rng_stats_s.fips_poker++;
                }
                if (result & FIPS_RNG_RUNS) {
                    all_rng_stats_s.fips_runs++;
                    curr_rng_stats_s.fips_runs++;
                }
                if (result & FIPS_RNG_LONGRUN) {
                    all_rng_stats_s.fips_longruns++;
                    curr_rng_stats_s.fips_longruns++;
	            fips_hard_fail = true;
                }
                if (result & FIPS_RNG_CONTINUOUS_RUN) {
                    all_rng_stats_s.fips_contruns++;
                    curr_rng_stats_s.fips_contruns++;
	            fips_hard_fail = true;
                }
		if ((relaxed_fips_) && (!fips_hard_fail)) {
                    all_rng_stats_s.good_fips_blocks++;
                    curr_rng_stats_s.good_fips_blocks++;
		    fipsPassed = true;
	        }
   	        else {
                    mem->datalen(0);
                    all_rng_stats_s.bad_fips_blocks++;
                    curr_rng_stats_s.bad_fips_blocks++;
		    fipsPassed = false;
                }
            }
            else {
                all_rng_stats_s.good_fips_blocks++;
                curr_rng_stats_s.good_fips_blocks++;
                fipsPassed = true;
            }
        }
    }

    log_s->write ((string)"HwEntropyThread::threadMain exiting...");

    return;
}

class DevRandThread : public SysThread
{
  public:
    DevRandThread (const char *  rndpath,
                   u32           writesz,
                   float         density,
                   u32           timeout) : rndpath_(rndpath), writesz_(writesz), density_(density), timeout_(timeout) {}
    virtual ~DevRandThread () {};
    virtual void threadMain (void);
  private:
    const char *  rndpath_;
    u32           writesz_;
    u32           timeout_;
    float         density_;
};

void
DevRandThread::threadMain () {
    int          result;
    u32          remaining;
    u32          buflen;
    bool         status;
    byte *       data;
    byte *       curr;
    float        entcount;
    LockedMem *  mem;
    LockedMem *  scrap;
    Timer        watch;

    scrap = new LockedMem;
    if (scrap->allocate (writesz_) == false) {
        log_s->write ((string)"Unable to allocate scrap buffer in call to DevRandThread::threadMain.  Exiting.");
        shutdown ();
        return;
    }

    struct timeval  maxwait;
    memset (&maxwait, 0, sizeof(maxwait));

    int fd = open(rndpath_, O_RDWR);
    if (fd < 0) return;

    struct {
            int  ent_count;
            int  size;
            // use maximum size to ensure adequate storage
            byte  data[RND_BLOCK_SIZE];
    } entropy;

    struct pollfd pfd = { fd, POLLOUT };

    log_s->write ((string)"DevRandThread::threadMain invoked.  Writesz: " + tostr(writesz_) +
                  " Density: " + tostr(density_) + " Timeout: " + tostr(timeout_));

    while (terminate_s == false) {
        watch.start ();
        if (RandXfer::getRandMem (mem) == false) {
            log_s->write ((string)"Fatal error trying to get RandXfer rand memory.  Exiting."); 
            shutdown ();
            return;
        }
        watch.stop ();
        all_rng_stats_s.random_starve_stats.add (watch);
        curr_rng_stats_s.random_starve_stats.add (watch);
        remaining = mem->datalen();
        data = mem->buffer();
        status = result = 0;
        while (remaining > 0 && (terminate_s == false) ) {
            poll(&pfd, 1, timeout_);
            all_rng_stats_s.total_devrnd_writeable++;
            curr_rng_stats_s.total_devrnd_writeable++;

            // deplete any scrap buffer before eating into xfer buffer
            curr   = entropy.data;
            buflen = writesz_;
            if (scrap->datalen() > 0) {
                memcpy (curr, scrap->buffer(), scrap->datalen());
                curr   += scrap->datalen();
                buflen -= scrap->datalen();
                scrap->datalen(0);
            }
            memcpy (curr, data, buflen);
            remaining -= buflen;
            data      += buflen;
            entcount = (float)writesz_ * (float)8 * density_;
            entropy.ent_count = (int)entcount;
            entropy.size = writesz_;
            if (remaining < writesz_) {
                memcpy (scrap->buffer(), data, remaining);
                scrap->datalen(remaining);
                remaining = 0;
            }

            result = ioctl(fd, RNDADDENTROPY, &entropy);
            if (result < 0) {
                // should never fail?
                log_s->write ((string)"Error adding entropy to random device pool in call to DevRandThread::threadMain.  Exiting.");
                shutdown ();
                return;
            }
            else {
                all_rng_stats_s.total_entadd_bytes += writesz_;
                curr_rng_stats_s.total_entadd_bytes += writesz_;
            }
        }
        mem->datalen(0);
    }
    close (fd);

    log_s->write ((string)"DevRandThread::threadMain exiting...");

    return;
}

class StatLogThread : public SysThread
{
  public:
    StatLogThread (const string & logDir,  u32 maxLogs,  u32 delay) : logdir_(logDir), maxlogs_(maxLogs), delay_(delay) {}
    virtual ~StatLogThread () {};
    virtual void threadMain (void);
  private:
    string  logdir_;
    u32     maxlogs_;
    u32     delay_;
};

void
StatLogThread::threadMain ()
{
    LogQueue *  queue = new LogQueue;
    Log *       log = 0;

    log_s->write ((string)"StatLogThread::threadMain invoked.");

    if (queue->initialize (logdir_, (string)"mtrngd-stats", maxlogs_) == false) {
        log_s->write ((string)"Unable to create stat log queue with path: " + logdir_ + ", exiting stat log thread.");
        return;
    }
    while (terminate_s == false) {
        if (queue->newLog (log) == true) {
            writeStats (log);
            queue->closeAll ();
        }
        sleep (delay_);
    }

    log_s->write ((string)"StatLogThread::threadMain exiting...");

    return;
}



const char *argp_program_version = "mtrngd-0.4";
const char *argp_program_bug_address = "martin peck <coderman@gmail.com>";

struct arguments {
    bool    daemon;
    char *  random_device;
    char *  entropy_source;
    char *  logdir;
    bool    xstore;
    u32     rng_density;
    u32     timeout;
    u32     poolsize;
    u32     writemark;
    u32     stats_interval;
    u32     statfile_max;
    bool    msr;
    char *  msr_device;
    u32     filter_size;
    u32     dcbias;
    bool    rawbits;
    u32     noise_src;
    bool    disable_rng;
    bool    forgiving;
};

static struct argp_option options[] = {
    { "foreground",      'f', 0, 0,       "Do not fork or redirect output" },
    { "background",      'b', 0, 0,       "Become a daemon (default)" },
    { "logdir",          'l', "dir", 0,   "Log file directory (default: /var/log/mtrngd)" },
    { "random-device",   'o', "file", 0,  "Kernel device used for random number output (default: /dev/random)" },
    { "entropy-source",  'r', "file", 0,  "Kernel device used to provide hardware random data (default: /dev/hwrandom)" },
    { "xstore",          'x', 0, 0,       "Use xstore instruction directly" },
    { "rng-density",     'z', "int", 0,   "Percentage of _effective_ truly random bits from hw rng (default: 80)" },
    { "timeout",         't', "int", 0,   "Interval written to random-device when the entropy pool is full, in seconds (default: 60)" },
    { "poolsize",        'p', "int", 0,   "Random pool size (default: 512)" },
    { "writemark",       'h', "int", 0,   "Event notification level for writers polling random device (default: 128)" },
    { "stats",           's', "int", 0,   "Statistics log write interval in seconds (default: 0, never)" },
    { "statmax",         'm', "int", 0,   "Maximum number of stats log files concurrent on disk (default: 0. no limit)" },
    { "msr",             'M', 0, 0,       "Use MSR registers to configure entropy source (C5* only)" },
    { "msr-device",      'F', "file", 0,  "MSR special device file (default: /dev/cpu/0/msr)" },
    { "filtersz",        'S', "int", 0,   "String filter continuous bit limit (C5* only. 8-63)" },
    { "dcbias",          'B', "int", 0,   "Entropy device DC bias (C5* only. 0-7)" },
    { "rawbits",         'R', 0, 0,       "Rawbits enable (C5* only)" },
    { "noisesrc",        'N', "int", 0,   "Hardware noise source (C5P only. 0,1 or 2)" },
    { "disable",         'D', 0, 0,       "Disable hardware entropy source on exit (c5* only)" },
    { "forgiving",         'g', 0, 0,       "Relax FIPS checks performed on a block.  poker and bit runs are ignored." },
    { 0 },
};

enum {
    POOL_MIN = 128,
    POOL_MAX = 4096,
    FILTER_MIN = 8,
    FILTER_MAX = 63,
    DCBIAS_MAX = 7,
    ENTSRC_MAX = 3,
};

static struct arguments default_arguments = {
    /* .daemon         */ true,
    /* .random_device  */ "/dev/random",
    /* .entropy_source */ "/dev/hwrandom",
    /* .logdir         */ "/var/log/mtrngd",
    /* .xstore         */ false,
    /* .rng_density    */ 80,
    /* .timeout        */ 60,
    /* .poolsize       */ 512,
    /* .writemark      */ 128,
    /* .stats_interval */ 0,
    /* .statfile_max   */ 0,
    /* .msr            */ false,
    /* .msr_device     */ "/dev/cpu/0/msr",
    /* .filter_size    */ 0,
    /* .dcbias         */ 0,
    /* .rawbits        */ false,
    /* .noise_src      */ 0,
    /* .disable_rng    */ false,
    /* .forgiving  */ false
};
struct arguments *arguments = &default_arguments;

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
    int           result;
    struct stat   filestat;
    struct arguments *   arguments = (struct arguments *)(state->input);

    switch(key) {
        case 'b':
            arguments->daemon = true; break;
        case 'f':
            arguments->daemon = false; break;
        case 'o': {
            if ((result = stat (arg, &filestat)) != 0) {
                argp_usage(state);
            }
            else {
                if (! S_ISCHR (filestat.st_mode)) {
                    argp_usage(state);
                }
                else {
                    arguments->random_device = arg;
                }
            }
            break;
        }
        case 'r': {
            if ((result = stat (arg, &filestat)) != 0) {
                argp_usage(state);
            }
            else {
                if (! S_ISCHR (filestat.st_mode)) {
                    argp_usage(state);
                }
                else {
                    arguments->entropy_source = arg;
                }
            }
            break;
        }
        case 'l': {
            if ((result = stat (arg, &filestat)) != 0) {
                argp_usage(state);
            }
            else {
                if (! S_ISDIR (filestat.st_mode)) {
                    argp_usage(state);
                }
                else {
                    arguments->logdir = arg;
                }
            }
            break;
        }
        case 'x':
            arguments->xstore = true; break;
        case 'z': {
            result = atoi (arg);
            if ( (! *arg) || (! isdigit(*arg)) || (result <= 0) || (result > 100) ) {
                argp_usage(state);
            }
            else {
                arguments->rng_density = result;
            }
            break;
        }
        case 't': {
            result = atoi (arg);
            if ( (! *arg) || (! isdigit(*arg)) || (result <= 0) ) {
                argp_usage(state);
            }
            else {
                arguments->timeout = result;
            }
            break;
        }
        case 'p': {
            result = atoi (arg);
            if ( (! *arg) || (! isdigit(*arg)) || (result < POOL_MIN) || (result > POOL_MAX) ) {
                argp_usage(state);
            }
            else {
                arguments->poolsize = result;
            }
            break;
        }
        case 'h': {
            result = atoi (arg);
            if ( (! *arg) || (! isdigit(*arg)) || (result <= 0) || (result > POOL_MAX) ) {
                argp_usage(state);
            }
            else {
                arguments->writemark = result;
            }
            break;
        }
        case 's': {
            result = atoi (arg);
            if ( (! *arg) || (! isdigit(*arg)) || (result < 0) ) {
                argp_usage(state);
            }
            else {
                arguments->stats_interval = result;
            }
            break;
        }
        case 'm': {
            result = atoi (arg);
            if ( (! *arg) || (! isdigit(*arg)) || (result < 0) ) {
                argp_usage(state);
            }
            else {
                arguments->statfile_max = result;
            }
            break;
        }
        case 'M':
            arguments->msr = true; break;
        case 'F': {
            if ((result = stat (arg, &filestat)) != 0) {
                argp_usage(state);
            }
            else {
                if (! S_ISCHR (filestat.st_mode)) {
                    argp_usage(state);
                }
                else {
                    arguments->msr_device = arg;
                }
            }
            break;
        }
        case 'S': {
            result = atoi (arg);
            if ( (! *arg) || (! isdigit(*arg)) || (result < FILTER_MIN) || (result > FILTER_MAX) ) {
                argp_usage(state);
            }
            else {
                arguments->filter_size = result;
            }
            break;
        }
        case 'B': {
            result = atoi (arg);
            if ( (! *arg) || (! isdigit(*arg)) || (result < 0) || (result > DCBIAS_MAX) ) {
                argp_usage(state);
            }
            else {
                arguments->dcbias = result;
            }
            break;
        }
        case 'R': 
            arguments->rawbits = true; break;
        case 'N': {
            result = atoi (arg);
            if ( (! *arg) || (! isdigit(*arg)) || (result < 0) || (result > ENTSRC_MAX) ) {
                argp_usage(state);
            }
            else {
                arguments->noise_src = result;
            }
            break;
        }
        case 'D':
            arguments->disable_rng = true; break;
        case 'g':
            arguments->forgiving = true; break;

      default:
          return ARGP_ERR_UNKNOWN;
    }
    return (0);
}

static struct argp s_argp = { options, parse_opt, NULL, NULL };

void
logStartupMsg ()
{
    const int  len = 1024;
    char * buf = new char[len];

    log_s->write ("mtrngd started.");

    const char * noise_src = "both";
    if (arguments->noise_src == 1) {
        noise_src = "A";
    }
    else if (arguments->noise_src == 2) {
        noise_src = "B";
    }

    snprintf (buf, len, "current configuration:\n\t"
        "run as daemon ......: %s\n\t"
        "random device ......: %s\n\t"
        "entropy source .....: %s\n\t"
        "log dir ............: %s\n\t"
        "rng density ........: %d%%\n\t"
        "timeout ............: %d seconds\n\t"
        "pool size ..........: %d\n\t"
        "write mark .........: %d\n\t"
        "stats interval .....: %d seconds\n\t"
        "stat file limit ....: %d files\n\t"
        "use MSR ............: %s\n\t"
        "MSR device .........: %s\n\t"
        "string filter len ..: %d\n\t"
        "DC bias ............: %d\n\t"
        "rawbit enabled .....: %s\n\t"
        "noise source .......: %s\n\t"
        "disable rng atexit .: %s\n\t"
        "forgiving fips .....: %s\n",
        (arguments->daemon) ? "true" : "false",
        arguments->random_device,
        (arguments->xstore) ? "xstore instruction" : arguments->entropy_source,
        arguments->logdir,
        arguments->rng_density,
        arguments->timeout,
        arguments->poolsize,
        arguments->writemark,
        arguments->stats_interval,
        arguments->statfile_max,
        (arguments->msr) ? "true" : "false",
        arguments->msr_device,
        arguments->filter_size,
        arguments->dcbias,
        (arguments->rawbits) ? "true" : "false",
        noise_src,
        (arguments->disable_rng) ? "yes" : "no",
        (arguments->forgiving) ? "yes" : "no"
    );

    log_s->write ((string)buf);
    delete [] buf;
}

void
setMsrConfig (ViaMsrRngConfig *  config)
{
    config->setRngEnable (true);
    config->setDcBias (arguments->dcbias);

    if (arguments->filter_size > 0) {
        config->setStrfltEnable (true);
        config->setStrfltLength (arguments->filter_size);
    }
    else {
        config->setStrfltEnable (false);
    }

    if (arguments->noise_src == 1) {
        config->setNoiseSrc (0);
    }
    else if (arguments->noise_src == 2) {
        config->setNoiseSrc (1);
    }
    else {
        config->setNoiseSrc (3);
    }

    if (arguments->rawbits) {
        config->setRawbitEnable (true);
    }
    else {
        config->setRawbitEnable (false);
    }
}


int
main (int c, char **v)
{
    const u32          rndbSize     = RND_BLOCK_SIZE;
    Entropy *          entropySrc   = 0;
    ViaMsrDevice *     msrDev       = 0;
    ViaMsrRngConfig *  msrConfig    = 0;
    HwEntropyThread *  hwThr        = 0;
    DevRandThread *    rndThr       = 0;
    StatLogThread *    statThr      = 0;

    argp_parse(&s_argp, c, v, 0, 0, arguments);

    if (arguments->daemon) {
        if (daemon(0,0) != 0) {
            cerr << "Unable to fork detached session." << endl;
            return (1);
        }
    }

    log_s = new Log;
    if (arguments->daemon) {
        string  logfile = (string)(arguments->logdir) + (string)"/mtrngd.log";
        if (log_s->open (logfile) == false) {
            cerr << "Unable to open application log file: " << logfile << ". Exiting." << endl;
            return (1);
        }
    }
    else {
        if (log_s->open (1) == false) {
            cerr << "Unable to open application log with stdout.  Exiting." << endl;
            return (1);
        }
    }

    logStartupMsg ();

    terminate_s = false;
    setFillSigMask ();

    if (arguments->msr) {
        msrDev = new ViaMsrDevice (arguments->msr_device);
        msrConfig = new ViaMsrRngConfig;
        if (msrDev->readViaMsr (*msrConfig) == false) {
            log_s->write ((string)"Unable to read MSR values from device: " + arguments->msr_device);
            return (1);
        }
        log_s->write ((string)"Current MSR config: " + msrConfig->asString());
        setMsrConfig (msrConfig);
        if (msrDev->writeViaMsr (*msrConfig) == false) {
            log_s->write ((string)"Unable to write MSR values to device: " + arguments->msr_device);
            return (1);
        }
        log_s->write ((string)"Updated to MSR config: " + msrConfig->asString());
    }

    if (RandXfer::initialize  (rndbSize) == false) {
        log_s->write ((string)"Unable to initialize thread entropy buffers");
        return (1);
    }
    if (arguments->xstore) {
        if (msrDev) {
            entropySrc = new XstoreRng (msrConfig, msrDev, rndbSize);
        }
        else {
            entropySrc = new XstoreRng (rndbSize);
        }
    }
    else {
	entropySrc = new DevRng (arguments->entropy_source, rndbSize);
    }

    hwThr = new HwEntropyThread (entropySrc, arguments->forgiving);
    hwThr->run ();

    rndThr = new DevRandThread (arguments->random_device,
                                arguments->poolsize - arguments->writemark,
                                (float)arguments->rng_density / 100.0,
                                arguments->timeout * 1000);
    rndThr->run ();

    if (arguments->stats_interval > 0) {
	statThr = new StatLogThread (arguments->logdir, arguments->statfile_max, arguments->stats_interval);
	statThr->run ();
    }

    string logfile = (string)(arguments->logdir) + (string)"/mtrngd.status";
    processSignals (logfile);

    shutdown ();
    return (0);
}

