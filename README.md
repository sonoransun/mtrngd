# mtrngd: Multi-threaded Random Number Generator Daemon

## Overview 

This application reads entropy from /dev/hwrandom, performs a FIPS check on the
output, and then feeds the entropy into /dev/random for use.

It can also use the MSR character device and xstore instruction in userspace to
obtain random bits from the VIA hardware entropy sources.

To build simply issue the "make" command in this directory.

The host init scripts may need to be modified to call the /etc/init.d/mtrgnd
script during the host boot process.  Please refer to this script for details
about command line arguments and logging.

Start/stop/restart supported by invoking the rc script as follows:

    /etc/init.d/mtrngd [start|stop|restart|status]

When using the status command you should see a message similar to the
following:

root@cpu0:~# /etc/init.d/mtrngd status
[Tue Aug 16 13:28:19-411849] Current MTRNGD Status:
 bad fips blocks ......: 758
   monobit failures ___: 0
   poker run failures _: 347
   bit run failures  __: 635
   long run failures __: 0
   cont run failures __: 0
 good fips blocks .....: 1314
 hwrng read bytes .....: 5180000
 entropy add bytes ....: 3279360
 random writeable cnt .: 8540
 hw entropy read stats ....:    min: 1288       avg:  1323      max: 2684       total: 2741904
 rng fips check stats .....:    min: 914        avg:  926       max: 2421       total: 1920127
 random recv starve stats .:    min: 4  avg:  26        max: 6772       total: 34555

All time values are in microseconds.


## Command line options:

---

It is reccommended that the following configuration be used as a conservative
use of hardware entropy with the best performance.

- patched hw_random.c built as a module in the latest 2.6 kernel
- modprobe hw_random rngsrc=0 rdelay=10 rawbit=0 strflt=1 fltlen=32
- mtrngd configured with 80% entropy density, using /dev/hwrandom

The init script contains hooks to set most of these values if desired. The full
ist of arguments available is as follows:

    -f or --foreground
        do not fork into the background and become a daemon.
  
    -b or --background
        fork into background and reparent under init.
  
    -l dir or --logdir dir
        directory where application log files will be written.
  
    -o file or --random-device file
        system random device to write entropy to. default /dev/random
  
    -r file or --entropy-source file
        entropy source - this is ignored if using xstore directly. default
        is /dev/hwrandom
  
    -x or --xstore
        use the xstore instruction directly instead of a hw entropy device
        file.
  
    -z int or --rng-density int
        rng entropy density as a percent. default is 80%.  while the VIA hw
        entropy source is estimated to produce over 99% pure entropy a
        conservative setting for this value ensures that dilution of the
        /dev/random entropy pool will not occur under normal operation.

    -t int or --timeout int
        timeout value in seconds for wait limit on /dev/random writeable.
        default is 60 seconds.

    -p int or --poolsize int
        size of /dev/random entropy pool.  this should always be set to the
        default of 512 unless you know exactly what you are doing.

    -h int or --writemark int
        write mark level for /dev/random entropy pool before gathered entropy
        is written into the pool.  this should be left at the default 128.

    -s int or --stats int
        write a statistics file to the log dir every int seconds.  the default
        is zero (0) which means do not write stats files at all.  these files
        are useful for monitoring the useage and performance of system entropy
        over time and thus it is suggested this be enabled unless disk space
        is absolutely critical.

    -m int or --statmax int
        maximum number of stats files to leave in the log directory.  this is
        only enabled if stats are being logged.  when the limit is reached the
        older files will be removed to make room for the newer files.  default
        is 0, no limit.  when the mtrngd restarts all old files will be removed.

    -M or --msr
        if using xstore this specifies use of the MSR device file to control
        entropy source configuration.  default is not enabled.

    -F file or --msr-device file
        the MSR device file to use for configuration of entropy registers.
        default /dev/cpu/0/msr.

    -S int or --filtersz int
        enable the string filter and set the limit to int.  this defaults to 0,
        not enabled, but the reccommended configuration is at least 32 bits.

    -B int or --dcbias int
        set the DC bias of the entropy sources to int.  must be a value between
        zero (0) and seven (7).  default is 0, not used.

    -R or --rawbits
        enable rawbit mode for entropy sources.  this should never be selected
        as the resulting entropy will usually fail multiple FIPS checks and
        lead to entropy starvation.

    -N int or --noisesrc int
        select specific noise source if multiple are present.  the C5XL core only
        has one entropy source while later revisions include two.  default is
        0, both.  can be set to 1 or 2 respectively.

    -D or --disable
        if using the MSR registers disable the RNG when application exits.

    -g or --forgiving
        ignore bit run and poker run FIPS failures as these occurr in good entropy
        with some statistical frequency.  it is strongly reccommended that this
        option NOT be used.  if throughput is of utmost concern and the entropy
        density is appropriately conservative this may be enabled without
        jeopardizing the integrity of the entropy pool.

