
#include "sr_context.h"

 /*
  for use by sr_post to avoid duplicate postings, sr_winnow to suppress duplicated, perhaps other consumers as well.

  is the use of the hash enough of a key?  dunno.
 */

#include <openssl/sha.h>
#include <time.h>
#include "uthash.h"

 /*
  get time string conversion routines.
 */
#include "sr_config.h"

#include "sr_cache.h"

extern char sumstr[];

void hash_print(unsigned char *hash) {
    for ( int i=0; i<SR_SUMHASHLEN ; i++ )
        fprintf( stderr, "%02x", hash[i] );
    fprintf( stderr, "\n" );

}

int sr_cache_check( struct sr_cache_t *cachep, char algo, void *ekey, char *path, char* partstr )
 /* 
   insert new item if it isn't in the cache.
   retun value:
       0 - present, so not added, but timestamp updated, so it doesn't age out so quickly.
       1 - was not present, so added to cache.
      -1 - key too long, could not be inserted anyways, so not present.
 */
{
     struct sr_cache_entry_t *c = NULL;
     struct sr_cache_entry_path_t *p;
  
     log_msg( LOG_DEBUG, "looking for: %s \nlooking for sum of: %s\n algo: %c, ekey: ", path, sumstr, algo );
     hash_print((unsigned char *)ekey);
     fprintf( stderr, "\n");
  
     fprintf( stderr, "e ready for HASH_FIND:");
     hash_print(ekey);
     fprintf( stderr, "hoho content is:\n");
  
     sr_cache_save( cachep, 1 );
  
     fprintf( stderr, "ok FIND: ");
     HASH_FIND( hh, cachep->data, ekey, SR_CACHEKEYSZ, c );
     fprintf( stderr, "cachep->data=%p, c=%p \n", cachep->data, c );
  
     if (!c) 
     {
         log_msg( LOG_DEBUG, "%s sum: %s was not in cache, adding\n", path, sumstr );
         c = (struct sr_cache_entry_t *)malloc(sizeof(struct sr_cache_entry_t));
         memset(c, 0, sizeof(struct sr_cache_entry_t) );

         memcpy(c->key, ekey, SR_CACHEKEYSZ );
         c->paths=NULL;
         HASH_ADD_KEYPTR( hh, cachep->data, c->key, SR_CACHEKEYSZ, c );
     }
  
     for ( p = c->paths; p ; p=p->next )
     { 
         /* compare path and partstr */
         log_msg( LOG_DEBUG, "sum was found in cache, looking at other attributes\n", path );
         if ( !strcmp(p->path, path) && !strcmp(p->partstr,partstr) ) {
             log_msg( LOG_DEBUG, "same path already there\n" );
             clock_gettime( CLOCK_REALTIME, &(p->created) ); /* refresh cache timestamp */
             return(0); /* found in the cache already */
         }
     }
  
     /* not found, so add path to cache entry */
     p = (struct sr_cache_entry_path_t *)malloc(sizeof(struct sr_cache_entry_path_t));
     memset(p, 0, sizeof(struct sr_cache_entry_path_t) );

     clock_gettime( CLOCK_REALTIME, &(p->created) );
     p->path = strdup(path);
     p->partstr = strdup(partstr);
     p->next = c->paths;
     c->paths = p;
     fprintf(cachep->fp,"%s %s %s %s\n", sr_hash2sumstr(c->key), sr_time2str( &(p->created) ), p->path, p->partstr );
     return(1);
}

void sr_cache_clean( struct sr_cache_t *cachep, float max_age )
 /* 
     remove entries in the cache not looked up in more than max_age seconds. 
 */
{
    struct sr_cache_entry_t *c, *tmpc;
    struct sr_cache_entry_path_t *e, *prev, *del;
    struct timespec since;
    signed long int diff;
    int npaths;

    memset( &since, 0, sizeof(struct timespec) );
    clock_gettime( CLOCK_REALTIME, &since );
    log_msg( LOG_DEBUG, "cleaning out entries. current time: %s\n", sr_time2str( &since ) );

    // subtracting max_age from now.
    since.tv_sec  -= (int)(max_age);
    diff =  (int) ((max_age-(int)(max_age))*1e9);
    if (diff > since.tv_nsec)
    {   // carry the one...
        since.tv_sec--;
        since.tv_nsec += 1e9;
    }
    since.tv_nsec -= diff;

    log_msg( LOG_DEBUG, "cleaning out entries older than: %s valu=%ld\n", sr_time2str( &since ), since.tv_sec );

    HASH_ITER(hh, cachep->data, c, tmpc )
    {
        log_msg( LOG_DEBUG, "hash, start\n" );
        e = c->paths; 
        prev=NULL;
        while ( e )
        {
           log_msg( LOG_DEBUG, "\tchecking %s, touched=%s difference: %ld\n", e->path, sr_time2str(&(e->created)) ,
                       e->created.tv_sec - since.tv_sec );
           if ( (e->created.tv_sec < since.tv_sec)  || 
                ((e->created.tv_sec == since.tv_sec) && (e->created.tv_nsec < since.tv_nsec)) 
              )
           {
              log_msg( LOG_DEBUG, "\tdeleting %s c->paths=%p, prev=%p, e=%p, e->next=%p\n", e->path,
                       c->paths, prev, e, e->next );
              del=e;

              if (!prev) {
                  c->paths = e->next;
                  prev=e;
              } else {
                  prev->next = e->next; 
              }
              e=e->next;

              free(del->path);
              free(del->partstr);
              free(del);
           } else  
           {
              prev=e;
              e=e->next;
           }
              
       }
   
       if (! (c->paths) ) 
       {
           log_msg( LOG_DEBUG, "hash, deleting\n" );
           HASH_DEL(cachep->data,c);
           free(c);
       } else  {
           npaths=0; 
           for ( e = c->paths; e ; e=e->next ) npaths++;
           log_msg( LOG_DEBUG, "hash, done. pop=%d\n", npaths );
       }
   }
}

void sr_cache_free( struct sr_cache_t *cachep)
 /* 
     remove all entries in the cache  (cleanup to discard.)
 */
{
    struct sr_cache_entry_t *c, *tmpc;
   struct sr_cache_entry_path_t *e, *del ;

    HASH_ITER(hh, cachep->data, c, tmpc )
    {
        HASH_DEL(cachep->data,c);
        e = c->paths; 
        while( e )
        {
            del=e;
            e=e->next; 
            free(del->path);
            free(del->partstr);
            free(del);
        }
        free(c);
    }
}

int sr_cache_save( struct sr_cache_t *cachep, int to_stdout)
 /* 
     write entries in the cache to disk.
     returns a count of paths written to disk.
 */
{
    struct sr_cache_entry_t *c, *tmpc;
    struct sr_cache_entry_path_t *e;
    FILE *f;
    int count=0;

    if (to_stdout) {
        f= stdout;
    } else {
        f = fopen( cachep->fn, "w" );
        if ( !f ) 
        {
            log_msg( LOG_ERROR, "failed to open cache file to save: %s\n", cachep->fn );
            return(0);
        }
    }
    HASH_ITER(hh, cachep->data, c, tmpc )
    {
       for ( e = c->paths; e ; e=e->next )
       {       
          fprintf(f,"%s %s %s %s\n", sr_hash2sumstr(c->key), sr_time2str( &(e->created) ), e->path, e->partstr );
          count++;
       }
    }
    if (!to_stdout) fclose(f);
    return(count);
}


#define load_buflen (SR_CACHEKEYSZ*2 + SR_TIMESTRLEN + PATH_MAX + 24)

static char buf[ load_buflen ];

struct sr_cache_entry_t *sr_cache_load( const char *fn)
 /* 
     create an sr_cache based on the content of the named file.     
 */
{
    struct sr_cache_entry_t *c, *cache;
    struct sr_cache_entry_path_t *p ;
    char *sum, *timestr, *path, *partstr;
    unsigned char key_val[SR_CACHEKEYSZ]; 
    FILE *f;
    int line_count=0;

    f = fopen( fn, "r" );
    if ( !f ) 
    {
        log_msg( LOG_DEBUG, "ERROR: failed to open cache file to load: %s\n", fn );
        return(NULL);
    }
    cache = NULL;

    while( fgets( buf, load_buflen, f ) )
    {
       line_count++;
       sum = strtok( buf, " " );
   
       if (!sum) 
       {
           log_msg( LOG_ERROR, "corrupt line in cache file %s: %s\n", fn, buf );
           continue;
       }

       timestr = strtok( NULL, " " );
   
       if (!timestr) 
       {
           log_msg( LOG_ERROR, "no timestring, corrupt line in cache file %s: %s\n", fn, buf );
           continue;
       }

       path = strtok( NULL, " " );
   
       if (!path) 
       {
           log_msg( LOG_ERROR, "no path, corrupt line in cache file %s: %s\n", fn, buf );
           continue;
       }

       partstr = strtok( NULL, " \n" );
   
       if (!partstr) 
       {
           log_msg( LOG_ERROR, "no partstr, corrupt line in cache file %s: %s\n", fn, buf );
           continue;
       }

       /*
       log_msg( LOG_DEBUG, "fields: sum=+%s+, timestr=+%s+, path=+%s+, partstr=+%s+\n", 
           sum, timestr, path, partstr );
       */
       
       memcpy( key_val, sr_sumstr2hash(sum), SR_CACHEKEYSZ );

       fprintf(stderr, "looking for: %s in cache\n", sr_hash2sumstr(key_val) );

       HASH_FIND( hh, cache, key_val, SR_CACHEKEYSZ, c);

       fprintf(stderr, "found it? c=%p\n", c );
       if (!c) {
           c = (struct sr_cache_entry_t *)malloc(sizeof(struct sr_cache_entry_t));
           if (!c) 
           {
               log_msg( LOG_ERROR, "out of memory reading cache file: %s, stopping at line: %s\n", fn, buf  );
               return(cache);
           }
           memset( c, 0, sizeof(struct sr_cache_entry_t));

           memcpy(c->key, key_val, SR_CACHEKEYSZ );
           fprintf( stderr, "key in the cache: " );
           hash_print(c->key);

           c->paths=NULL;
           HASH_ADD_KEYPTR( hh, cache, c->key, SR_CACHEKEYSZ, c );
           
       }
       /* assert, c != NULL */
       fprintf( stderr, "passed C optional malloc.\n" );

       /* skip if path already present */
       for( p=c->paths; p; p=p->next )
           if ( !strcmp(p->path,path) && !strcmp(p->partstr,partstr) )
               break;
       if (p) 
       {
           fprintf( stderr, "path already there.\n" );
           continue;
       }
       /* add path to cache entry */
       p = (struct sr_cache_entry_path_t *)malloc(sizeof(struct sr_cache_entry_path_t));
       if (!p) 
       {
           log_msg( LOG_ERROR, "out of memory 2, reading cache file: %s, stopping at line: %s\n", fn, buf  );
           return(cache);
       }
       fprintf( stderr, "passed P malloc.\n" );
       memset( p, 0, sizeof(struct sr_cache_entry_path_t) );

       memset( &(p->created), 0, sizeof(struct timespec) );
       memcpy( &(p->created), sr_str2time( timestr ), sizeof(struct timespec) );
       p->path = strdup(path);
       p->partstr = strdup(partstr);
       p->next = c->paths;
       c->paths = p;

       fprintf( stderr, "Next!.\n" );
    }
    fclose(f);
    fprintf( stderr, " _load done!.\n" );
    return(cache);
}

struct sr_cache_t *sr_cache_open( const char *fn )
{
    struct sr_cache_t *c;

    c = (struct sr_cache_t *)malloc(sizeof(struct sr_cache_t));
    memset(c,0,sizeof(struct sr_cache_t));
    c->data = sr_cache_load(fn);
    c->fn =  strdup(fn);
    c->fp = fopen(fn,"a");

    fprintf( stderr, "sr_cache_open loaded:\n" );
    sr_cache_save( c, 1 ); // FIXME, debug
    fprintf( stderr, "sr_cache_open done.\n" );
    return(c);
}

void sr_cache_close( struct sr_cache_t *c )
{
   if (!c) return;

   sr_cache_free( c );
   fclose(c->fp);
   free(c->fn);
   free(c);
}


