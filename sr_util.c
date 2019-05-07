

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdio.h>
#include <stdlib.h>
#include <openssl/md5.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <linux/limits.h>

#include "sr_util.h"

int     logfd = STDERR_FILENO;
time_t  logbase;
/* default values set in sr_config.c:sr_config_init() */
char    *logfn;
char    logfn_ts[PATH_MAX];
int     loglevel;
int     logmode;
int     logrotate;
int     logrotate_interval;

time_t  t;
struct  tm tc;

#ifndef SR_DEBUG_LOGS
void log_msg(int prio, const char *format, ...)
{
    char *p;
    va_list ap
    va_list apforsprintf;

    va_start(ap, format);

    if (prio > loglevel) return;

    switch (prio)
    {
        case LOG_DEBUG    : p = "DEBUG"; break;
        case LOG_INFO     : p = "INFO"; break;
        case LOG_WARNING  : p = "WARNING"; break;
        case LOG_ERROR    : p = "ERROR"; break;
        case LOG_CRITICAL : p = "CRITICAL"; break;
        default           : p = "UNKNOWN";
    }

    time(t);
    localtime_r(t, &tc);

    /* log rotation */
    if ( logfn && ( (t-logbase) > logrotate_interval ) )
    {
      char buf[PATH_MAX];
      int buflen;
      struct tm ystm;
      time_t yt;

      yt = ts.tv_sec-(23*3600);
      localtime_r(&(yt),&ystm);
      strcpy( buf, logfn );
      buflen = strlen(buf) ;

      snprintf( buf+buflen, PATH_MAX-buflen, ".%04d-%02d-%02d",  ystm.tm_year+1900, ystm.tm_mon+1, ystm.tm_mday );
      if ( !rename( logfn, buf ) ) {
         close( logfd );
         logfd = open( logfn, O_WRONLY|O_CREAT|O_APPEND, logmode );
      }

      logbase = tc.tm_mday;

      /* remove logs that are too old */
      yt = ts.tv_sec - logrotate;

      gmtime_r(&(yt),&ystm);
      strcpy( buf, logfn );
      buflen = strlen(buf) ;

      snprintf( buf+buflen, PATH_MAX-buflen, ".%04d-%02d-%02d",  ystm.tm_year+1900, ystm.tm_mon+1, ystm.tm_mday );
      unlink( buf );
    }
    /* logging */
    dprintf( logfd, "%04d-%02d-%02d %02d:%02d:%02d,%03d [%s] ", tc.tm_year+1900, tc.tm_mon+1,
        tc.tm_mday, tc.tm_hour, tc.tm_min, tc.tm_sec, (int)(ts.tv_nsec/1e6), p );
    va_copy( apforsprintf, ap);
    va_end(ap);
    vdprintf( logfd, format, apforsprintf);
}
#endif

void log_set_fn(struct tm *tc)
{
    char *p;

    strcpy(logfn_ts, logfn);
    p = logfn_ts + strlen(logfn);

    int lri = logrotate_interval;
    if        ( !(lri % (24*60*60)) ) {
        /* daily resolution */
        strcpy(p, "%04d-%02d-%02d");

    } else if ( !(lri % (60*60)) ) {
        /* hourly resolution */
        strcpy(p, "%04d-%02d-%02d");

    } else if ( !(lri % (60)) ) {
        /* minute resolution */
        strcpy(p, "%04d-%02d-%02d");

    } else {
        /* second resolution */
        strcpy(p, "%04d-%02d-%02d");
    }

    /* FIXME commiting only to ensure data safety during VM manipulations */
}

void log_setup(const char *fn, mode_t mode, int level, int lr, int lri )
{
#ifndef SR_DEBUG_LOGS
   logfn = strdup( fn );
   logmode = mode;
   logrotate = lr;
   logrotate_interval = lri;

   time(t);
   localtime_r(t, &tc);
   logbase = t;

   logfd = open( logfn, O_WRONLY|O_CREAT|O_APPEND, logmode );
   loglevel = level;
#endif
   return;
}

void log_cleanup() 
{
#ifndef SR_DEBUG_LOGS
   free( logfn );
   logfn = NULL;

   if ( (logfd != -1) && ( logfd != STDERR_FILENO))
        close( logfd );
   logfd = STDERR_FILENO;
#endif
   return;
}

void daemonize(int close_stdout)
/* 
   fork child,  parent then exits.  child returns with proper daemon prep done.
 */
{
     pid_t pid;
     pid_t sid;

     pid = fork();

     if ( pid < 0 )
     {
        log_msg( LOG_CRITICAL, "fork failed, cannot launch as daemon\n" );
        exit(1);
     }
     if ( pid > 0 )
     {
        fprintf( stderr, "parent exiting normally, rest is upto the child pid: %d\n", pid );
        exit(0);
     }
     // child processing.

     log_msg( LOG_DEBUG, "child daemonizing start\n" );
     sid = setsid();
     if (sid < 0)
     {  
        log_msg( LOG_WARNING, "daemonizing, setsid errord, failed to completely dissociate from login process\n" );
     } 

     if (logfd == 2)
     {
        log_msg( LOG_CRITICAL, "to run as daemon log option must be set.\n" );
        exit(1);
     }

     close(0); 
     if (close_stdout) 
     {
         close(1);
         dup2(logfd, STDOUT_FILENO);
     }
     close(2);
     dup2(logfd, STDERR_FILENO);

     log_msg( LOG_DEBUG, "child daemonizing complete.\n" );
}



/* size of buffer used to read the file content in calculating checksums.
 */
#define SUMBUFSIZE (4096*1024)

// SHA512 being the longest digest...
char sumstr[ SR_SUMSTRLEN ];

unsigned char sumhash[SR_SUMHASHLEN]; 


int get_sumhashlen( char algo )
{
  switch(algo) {
    case 'd' : case 'n' : 
        return(MD5_DIGEST_LENGTH+1);

    case '0': 
        return(4+1);

    case 'p' : case 's' : case 'L' : case 'R' : 
        return(SHA512_DIGEST_LENGTH+1);

    case 'z' :
        return(2);

    default: 
        return(0);
  }
}


char *set_sumstr( char algo, char algoz, const char* fn, const char* partstr, char *linkstr,
          unsigned long block_size, unsigned long block_count, unsigned long block_rem, unsigned long block_num 
     )
 /* 
   return a correct sumstring (assume it is big enough)  as per sr_post(7)
   algo = 
     '0' - no checksum, value is random. -> now same as N.
     'd' - md5sum of block.
     'n' - md5sum of filename (fn).
     'L' - now sha512 sum of link value.
     'p' - md5sum of filename (fn) + partstr.
     'R' - no checksum, value is random. -> now same as N.
     's' - sha512 sum of block.
     'z' - downstream should recalculate with algo that is argument.
   block starts at block_size * block_num, and ends 
  */
{
   MD5_CTX md5ctx;
   SHA512_CTX shactx;

   static int fd;
   static char buf[SUMBUFSIZE];
   long bytes_read ; 
   long how_many_to_read;
   const char *just_the_name=NULL;

   unsigned long start = block_size * block_num ;
   unsigned long end;
  
   end = start + ((block_num < (block_count -(block_rem!=0)))?block_size:block_rem) ;
 

   memset( sumhash, 0, SR_SUMHASHLEN );
   sumhash[0]=algo;

   switch (algo) {

   case '0' : 
       sprintf( sumstr, "%c,%03ld", algo, random()%1000 );
       return(sumstr);
       break;

   case 'd' :
       MD5_Init(&md5ctx);

       // keep file open through repeated calls.
       //fprintf( stderr, "opening %s to checksum\n", fn );

       if ( ! (fd > 0) ) fd = open( fn, O_RDONLY );
       if ( fd < 0 ) 
       { 
           fprintf( stderr, "unable to read file for checksumming\n" );
           strcpy(sumstr+3,"deadbeef0");
           return(NULL);
       } 
       lseek( fd, start, SEEK_SET );
       //fprintf( stderr, "checksumming start: %lu to %lu\n", start, end );
       while ( start < end ) 
       {
           how_many_to_read= ( SUMBUFSIZE < (end-start) ) ? SUMBUFSIZE : (end-start) ;

           bytes_read=read(fd,buf, how_many_to_read );           
           if ( bytes_read > 0 ) 
           {
              MD5_Update(&md5ctx, buf, bytes_read );
              start += bytes_read;
           } else {
              fprintf( stderr, "error reading %s for MD5\n", fn );
              close(fd);
              fd=0;
              return(NULL);
           } 
       }

       // close fd, when end of file reached.
       if ((block_count == 1)  || ( end >= ((block_count-1)*block_size+block_rem))) 
       { 
             close(fd);
             fd=0;
       }

       MD5_Final(sumhash+1, &md5ctx);
       return(sr_hash2sumstr(sumhash)); 
       break;

   case 'n' :
       MD5_Init(&md5ctx);
       just_the_name = rindex(fn,'/')+1;
       if (!just_the_name) just_the_name=fn;
       MD5_Update(&md5ctx, just_the_name, strlen(just_the_name) );
       MD5_Final(sumhash+1, &md5ctx);
       return(sr_hash2sumstr(sumhash)); 
       break;
       
   case 'L' : // symlink case
        just_the_name=linkstr;       
        SHA512_Init(&shactx);
        SHA512_Update(&shactx, linkstr, strlen(linkstr) );
        SHA512_Final(sumhash+1, &shactx);
        return(sr_hash2sumstr(sumhash)); 

   case 'R' : // null, or removal.
        just_the_name = rindex(fn,'/')+1;
        if (just_the_name<(char*)2) just_the_name=fn;
        SHA512_Init(&shactx);
        SHA512_Update(&shactx, just_the_name, strlen(just_the_name) );
        SHA512_Final(sumhash+1, &shactx);
        return(sr_hash2sumstr(sumhash)); 

   case 'p' :
       SHA512_Init(&shactx);
       just_the_name = rindex(fn,'/')+1;
       if (just_the_name<(char*)2) just_the_name=fn;
       strcpy( buf, just_the_name);
       sprintf( buf , "%s%c,%lu,%lu,%lu,%lu", just_the_name, algo, block_size, block_count, block_rem, block_num );
       SHA512_Update(&shactx, buf, strlen(buf) );
       SHA512_Final(sumhash+1, &shactx);
       return(sr_hash2sumstr(sumhash)); 

   case 's' : 
       SHA512_Init(&shactx);

       // keep file open through repeated calls.
       if ( ! (fd > 0) ) fd = open( fn, O_RDONLY );
       if ( fd < 0 ) 
       { 
           fprintf( stderr, "unable to read file for SHA checksumming\n" );
           return(NULL);
       } 
       lseek( fd, start, SEEK_SET );
       //fprintf( stderr, "DBG checksumming start: %lu to %lu\n", start, end );
       while ( start < end ) 
       {
           how_many_to_read= ( SUMBUFSIZE < (end-start) ) ? SUMBUFSIZE : (end-start) ;

           bytes_read=read(fd,buf, how_many_to_read );           

            //fprintf( stderr, "checksumming how_many_to_read: %lu bytes_read: %lu\n", 
            //   how_many_to_read, bytes_read );

           if ( bytes_read >= 0 ) 
           {
              SHA512_Update(&shactx, buf, bytes_read );
              start += bytes_read;
           } else {
              fprintf( stderr, "error reading %s for SHA\n", fn );
              close(fd);
              fd=0;
              return(NULL);
           } 
       }

       // close fd, when end of file reached.
       if ((block_count == 1)  || ( end >= ((block_count-1)*block_size+block_rem))) 
       { 
             close(fd);
             fd=0;
       }
       SHA512_Final(sumhash+1, &shactx);
       return(sr_hash2sumstr(sumhash)); 

   case 'z':
       sumhash[1]=algoz;
       sumhash[2]='\0';
       return( sr_hash2sumstr(sumhash) );

   default:
       fprintf( stderr, "sum algorithm %c unimplemented\n", algo );
       return(NULL);
   }

}

char nibble2hexchr( int i )

{
   unsigned char c =  (unsigned char)(i & 0xf);
   return( (char)((c < 10) ? ( c + '0' ) : ( c -10 + 'a' ) ));
}

int hexchr2nibble( char c )
 /* return ordinal value of digit assuming a character set that has a-f sequential in both lower and upper case.
    kind of based on ASCII, because numbers are assumed to be lower in collation than upper and lower case letters.
  */
{
    if ( c < ':' ) return(c - '0');
    if ( c < 'G' ) return(c - 'A' + 10);
    if ( c < 'g' ) return(c - 'a' + 10);
    return(-1);
}

unsigned char *sr_sumstr2hash( const char *s )
{
    int i;
    if (!s) return(NULL);
    memset( sumhash, 0, SR_SUMHASHLEN );
    sumhash[0]=s[0];

    if ( s[0] == 'z' ) 
    {
       sumhash[1] = s[2];
       return(sumhash);
    }
    
    for ( i=1; ( i < get_sumhashlen(s[0]) ) ; i++ )
    {
        sumhash[i] = (unsigned char)((hexchr2nibble(s[i<<1]) << 4) + hexchr2nibble(s[(i<<1)+1]));
    }
    return(sumhash);
}


char *sr_hash2sumstr( const unsigned char *h )
{
  int i;
  memset( sumstr, 0, SR_SUMSTRLEN );
  sumstr[0] = h[0];
  sumstr[1] = ',';

  if ( sumstr[0] == 'z' )
  {
      sumstr[2] = h[1];
      sumstr[3] = '\0';;
      return(sumstr); 
  }

  for(i=1; i < get_sumhashlen(h[0]); i++ )
  {
     sumstr[ i*2   ] = nibble2hexchr( h[i]>>4 );
     sumstr[ i*2+1 ] = nibble2hexchr( h[i] );
  }
  sumstr[2*i]='\0';
  return(sumstr);
}

static char time2str_result[SR_TIMESTRLEN];

char *sr_time2str( struct timespec *tin ) 
{
   /* turn a timespec into an 18 character sr_post(7) conformant time stamp string.
      if argument is NULL, then the string should correspond to the current system time.
    */
   struct tm s;
   time_t when;
   struct timespec ts;
   long nsec;
   char nsstr[30];
   int nsl;

   memset( &s, 0, sizeof(struct tm));
   memset( &ts, 0, sizeof(struct timespec));

   if ( tin ) {
     when = tin->tv_sec;
     nsec = tin->tv_nsec ;
   } else {
     clock_gettime( CLOCK_REALTIME , &ts);
     when = ts.tv_sec;
     nsec = ts.tv_nsec ;
   }

   if (nsec > 0)
   {
     nsstr[0]='\0';
     sprintf( nsstr, "%09ld", nsec );

     // remove trailing 0's, not relevant after a decimal place.
     nsl=strlen(nsstr)-1;
     while ( nsstr[nsl] == '0' ) 
     {
       nsstr[nsl]='\0';
       nsl--;
     }
   } else {
     strcpy( nsstr, "0" );
   }


   gmtime_r(&when,&s);
   /*                         YYYY  MM  DD  hh  mm  ss */
   sprintf( time2str_result, "%04d%02d%02d%02d%02d%02d.%s", s.tm_year+1900, s.tm_mon+1,
        s.tm_mday, s.tm_hour, s.tm_min, s.tm_sec, nsstr );
   return(time2str_result);
}

int ipow(int base, int exp)
/* all hail stack overflow: 
   https://stackoverflow.com/questions/101439/the-most-efficient-way-to-implement-an-integer-based-power-function-powint-int
 */
{
    int result = 1;
    while (exp)
    {
        if (exp & 1)
            result *= base;
        exp >>= 1;
        base *= base;
    }

    return result;
}

struct timespec ts;

struct timespec *sr_str2time( char *s )
  /* inverse of above: convert SR_TIMESTRLEN character string into a timespec.
    
   */
{
  struct tm tm;
  memset( &tm, 0, sizeof(struct tm));
  memset( &ts, 0, sizeof(struct timespec));
  int dl; // length of decimal string.

  strptime( s, "%Y%m%d%H%M%S", &tm);
  ts.tv_sec = timegm(&tm);
  
  dl = strlen(s+15); // how many digits after decimal point?
  ts.tv_nsec = atol(s+15) * ipow( 10, dl );
  return(&ts);
}


