/* vim:set ft=c ts=2 sw=2 sts=2 et cindent: */

/*
 * Usage info after license block.
 *
 * This code is by Peter Silva copyright (c) 2017 part of MetPX.
 * copyright is to the Government of Canada. code is GPLv2
 *
 * based on a amqp_sendstring from rabbitmq-c package
 * the original license is below:
 */

/* 
  Minimal c implementation to allow posting of sr_post(7) messages.
  It has a lot of limitations, and no error checking for now.

  how to use:

  In a shell, to use an sr_config(7) style configuration file:
  set the SR_POST_CONFIG environment variable to the name of the
  file to use.

 
 limitations:
    - Doesn't support document_root, absolute paths posted.
    - Doesn't support cache.
    - does support csv for url, to allow load spreading.
    - seems to be about 30x faster than python version.

 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <sys/types.h>
#include <sys/stat.h>



#include <unistd.h>
#include <fcntl.h>
#include <linux/limits.h>

#include <openssl/md5.h>
#include <openssl/sha.h>

#include <stdint.h>
#include <amqp_tcp_socket.h>
#include <amqp_ssl_socket.h>
#include <amqp.h>
#include <amqp_framing.h>

#include "sr_context.h"

// needed for sr_post_message.
#include "sr_consume.h"

/*
 Statically assign the maximum number of headers that can be included in a message.
 just picked a number.  I remember picking a larger one before, and it bombed, don't know why.

 */
#define HDRMAX (255)


amqp_table_entry_t headers[HDRMAX];

int hdrcnt = 0 ;

void header_reset() {
    hdrcnt--;
    for(; (hdrcnt>=0) ; hdrcnt--)
    {
        headers[hdrcnt].key = amqp_cstring_bytes("");
        headers[hdrcnt].value.kind = AMQP_FIELD_KIND_VOID;
        headers[hdrcnt].value.value.bytes = amqp_cstring_bytes("");
    }
    hdrcnt=0;
}

void amqp_header_add( char *tag, const char * value ) {

  if ( hdrcnt >= HDRMAX ) 
  {
     log_msg( LOG_ERROR, "ERROR too many headers! ignoring %s=%s\n", tag, value );
     return;
  }
  headers[hdrcnt].key = amqp_cstring_bytes(tag);
  headers[hdrcnt].value.kind = AMQP_FIELD_KIND_UTF8;
  headers[hdrcnt].value.value.bytes = amqp_cstring_bytes(value);
  hdrcnt++;
  log_msg( LOG_DEBUG, "Adding header: %s=%s hdrcnt=%d\n", tag, value, hdrcnt );
}

void set_url( char* m, char* spec ) 
  /* Pick a URL from the spec (round-robin) copy it to the given buffer
   */
{
  static const char* cu_url = NULL;
  char *sp;

  if ( strchr(spec,',') ) {
     //log_msg( LOG_DEBUG, "1 picking url, set=%s, cu=%s\n", spec, cu_url );
     if (cu_url) {
         cu_url = strchr(cu_url,','); // if there is a previous one, pick the next one.
         //log_msg( LOG_DEBUG, "2 picking url, set=%s, cu=%s\n", spec, cu_url );
     }
     if (cu_url) {
         cu_url++;                    // skip to after the comma.
         //log_msg( LOG_DEBUG, "3 picking url, set=%s, cu=%s\n", spec, cu_url );
     } else {
         cu_url = spec ;                // start from the beginning.
         //log_msg( LOG_DEBUG, "4 picking url, set=%s, cu=%s\n", spec, cu_url );
     }
     sp=strchr(cu_url,',');
     if (sp) strncpy( m, cu_url, sp-cu_url );
     else strcpy( m, cu_url );
  } else  {
     strcpy( m, spec );
  }
}

unsigned long int set_blocksize( long int bssetting, size_t fsz )
{
      unsigned long int tfactor =  (50*1024*1024) ;

      switch( bssetting )
      {
        case 0: // autocompute 
             if ( fsz > 100*tfactor ) return( 10*tfactor );
             else if ( fsz > 10*tfactor ) return( (unsigned long int)( (fsz+9)/10) );
             else if ( fsz > tfactor ) return( (unsigned long int)( (fsz+2)/3) ) ;
             else return(fsz);
             break;

        case 1: // send file as one piece.
             return(fsz);
             break;

       default: // partstr=i
             return(bssetting);
             break;
      }

}


void sr_post_message( struct sr_context *sr_c, struct sr_message_t *m )
{
    char message_body[1024];
    char smallbuf[256];
    amqp_table_t table;
    amqp_basic_properties_t props;
    signed int status;
    struct sr_header_t *uh;


    if ( sr_c->cfg->cache > 0 ) 
    { 
           status = sr_cache_check( sr_c->cfg->cachep, m->sum[0], (unsigned char*)(m->sum), m->path, sr_message_partstr(m) ) ; 
           log_msg( LOG_DEBUG, "post_message cache_check result=%d\n", status );
           if (!status) return; // cache hit.
    }

    strcpy( message_body, m->datestamp);
    strcat( message_body, " " );
    strcat( message_body, m->url );
    strcat( message_body, " " );
    strcat( message_body, m->path );
    strcat( message_body, " \n" );
 
    header_reset();

    amqp_header_add( "from_cluster", m->from_cluster );

    if (( m->sum[0] != 'R' ) && ( m->sum[0] != 'L' ))
    {
       amqp_header_add( "parts", sr_message_partstr(m) );
       amqp_header_add( "atime", m->atime );
       sprintf( smallbuf, "%04o", m->mode );
       amqp_header_add( "mode", smallbuf );
       amqp_header_add( "mtime", m->mtime );
    }

    if ( m->sum[0] == 'L' )
    {
       amqp_header_add( "link", m->link );
    }

    amqp_header_add( "sum", sr_hash2sumstr((unsigned char*)(m->sum)) );
    amqp_header_add( "to_clusters", m->to_clusters );

    for(  uh=m->user_headers; uh ; uh=uh->next )
        amqp_header_add(uh->key, uh->value);

    table.num_entries = hdrcnt;
    table.entries=headers;

    props._flags = AMQP_BASIC_HEADERS_FLAG | AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
    props.content_type = amqp_cstring_bytes("text/plain");
    props.delivery_mode = 2; /* persistent delivery mode */
    props.headers = table;

    status = amqp_basic_publish(sr_c->cfg->post_broker->conn, 1, amqp_cstring_bytes(sr_c->cfg->post_broker->exchange), 
              amqp_cstring_bytes(m->routing_key), 0, 0, &props, amqp_cstring_bytes(message_body));

    if ( status < 0 ) 
        log_msg( LOG_ERROR, "sr_%s: publish of message for  %s%s failed.\n", sr_c->cfg->progname, m->url, m->path );
    else 
        log_msg( LOG_INFO, "published: %s\n", sr_message_2log(m) );

}

int sr_file2message_start(struct sr_context *sr_c, const char *pathspec, struct stat *sb, struct sr_message_t *m ) 
/*
  reading a file, initialize the message that corresponds to it. Return the number of messages to post entire file.
 */
{
  char  *drfound;
    char  fn[PATH_MAXNUL];
  int lasti;
  int   linklen;
  char *linkp;
  char  linkstr[PATH_MAXNUL];
   
    if (*pathspec != '/' ) // need absolute path.
    { 
        getcwd( fn, PATH_MAX);
        strcat( fn, "/" );
        strcat( fn, pathspec);
    } else {
        if ( sr_c->cfg->realpath ) 
            realpath( pathspec, fn );
        else
            strcpy( fn, pathspec );
    }

  if ( (sr_c->cfg!=NULL) && sr_c->cfg->debug )
  {
     log_msg( LOG_DEBUG, "sr_%s file2message called with: %s sb=%p islnk=%d, isdir=%d, isreg=%d\n", 
         sr_c->cfg->progname, fn, sb, sb?S_ISLNK(sb->st_mode):0, sb?S_ISDIR(sb->st_mode):0,sb?S_ISREG(sb->st_mode):0 );
  }
  if ( sb && S_ISDIR(sb->st_mode) ) return(0); // cannot post directories.

  strcpy( m->path, fn );
  if (sr_c->cfg->documentroot) 
  {
      drfound = strstr(fn, sr_c->cfg->documentroot ); 
   
      if (drfound) 
      {
          drfound += strlen(sr_c->cfg->documentroot) ; 
          strcpy( m->path, drfound );
      } 
  } 
  // FIXME: 255? AMQP_SS_LEN limit?
  strcpy( m->routing_key, sr_c->cfg->topic_prefix );

  strcat( m->routing_key, m->path );
  lasti=0;
  for( int i=strlen(sr_c->cfg->topic_prefix) ; i< strlen(m->routing_key) ; i++ )
  {
      if ( m->routing_key[i] == '/' ) 
      {
           if ( lasti > 0 ) 
           {
              m->routing_key[lasti]='.';
           }
           lasti=i;
      }
  }
  m->routing_key[lasti]='\0';

  strcpy( m->datestamp, sr_time2str(NULL));
  strcpy( m->to_clusters, sr_c->cfg->to );

  m->parts_blkcount=1;
  m->parts_rem=0;
  m->parts_num=0;

  m->user_headers=sr_c->cfg->user_headers;

  m->sum[0]= sr_c->cfg->sumalgo;


  if ( !sb ) 
  {
      if ( ! ((sr_c->cfg->events)&SR_DELETE) ) return(0); // not posting deletes...
      m->sum[0]='R';
  } else if ( S_ISLNK(sb->st_mode) ) 
  {
      if ( ! ((sr_c->cfg->events)&SR_LINK) ) return(0); // not posting links...

      strcpy( m->atime, sr_time2str(&(sb->st_atim)));
      strcpy( m->mtime, sr_time2str(&(sb->st_mtim)));
      m->mode = sb->st_mode & 07777 ;

      m->sum[0]='L';
      linklen = readlink( fn, linkstr, PATH_MAX );
      linkstr[linklen]='\0';
      if ( sr_c->cfg->realpath ) 
      {
          linkp = realpath( linkstr, m->link );
          if (!linkp) 
          {
               log_msg( LOG_ERROR, "sr_%s unable to obtain realpath for %s\n", sr_c->cfg->progname, fn );
               return(0);
          }
      } else {
         strcpy( m->link, linkstr ); 
      }

  } else if (S_ISREG(sb->st_mode)) 
  {   /* regular files, add mode and determine block parameters */

      if ( ! ((sr_c->cfg->events)&(SR_CREATE|SR_MODIFY)) ) return(0);

      if ( access( fn, R_OK ) ) return(0); // will not be able to checksum if we cannot read.

      strcpy( m->atime, sr_time2str(&(sb->st_atim)));
      strcpy( m->mtime, sr_time2str(&(sb->st_mtim)));
      m->mode = sb->st_mode & 07777 ;

      m->parts_blksz  = set_blocksize( sr_c->cfg->blocksize, sb->st_size );
      m->parts_s = (m->parts_blksz < sb->st_size )? 'i':'1' ;

      if ( m->parts_blksz == 0 ) {
          m->parts_rem = 0;
      } else {
          m->parts_rem = sb->st_size%(m->parts_blksz) ;
          m->parts_blkcount = ( sb->st_size / m->parts_blksz ) + ( m->parts_rem?1:0 );
      }

  } 
  return(m->parts_blkcount);
}

struct sr_message_t *sr_file2message_seq(const char *pathspec, int seq, struct sr_message_t *m ) 
/*
  Given a message from a "started" file, the prototype message, and a sequence number ( sequence is number of blocks of partsze )
  return the adjusted prototype message.  (requires reading part of the file to checksum it.)
 */
{
      m->parts_num = seq;

      strcpy( m->sum, 
              set_sumstr( m->sum[0], pathspec, NULL, m->link, m->parts_blksz, m->parts_blkcount, m->parts_rem, m->parts_num ) 
            ); 

      if ( !(m->sum) ) 
      {
         log_msg( LOG_ERROR, "file2message_seq unable to generate %c checksum for: %s\n", m->parts_s, pathspec );
         return(NULL);
      }
  return(m);
}


void sr_post(struct sr_context *sr_c, const char *pathspec, struct stat *sb ) 
{
  static struct sr_message_t m;
  int numblks;

  strcpy( m.to_clusters, sr_c->cfg->to );
  strcpy( m.from_cluster, sr_c->cfg->post_broker->hostname );
  strcpy( m.source,  sr_c->cfg->post_broker->user );
  set_url( m.url, sr_c->cfg->url );
  m.user_headers = sr_c->cfg->user_headers;

  // report...
  // FIXME: duration, consumingurl, consuminguser, statuscode?
  numblks = sr_file2message_start( sr_c, pathspec, sb, &m );

  for( int blk=0; (blk < numblks); blk++ )
  {
      if ( sr_file2message_seq(pathspec, blk, &m ) ) 
          sr_post_message( sr_c, &m );
  }

}

void sr_post_rename(struct sr_context *sr_c, const char *oldname, const char *newname)
/*
   assume actual rename is completed, so newname exists.
 */
{
  struct stat sb;
  struct sr_header_t first_user_header;

  if ( lstat( newname, &sb ) ) 
  {
     log_msg( LOG_ERROR, "sr_%s rename: %s cannot stat.\n", sr_c->cfg->progname, newname );
     return;
  }

  first_user_header.next = sr_c->cfg->user_headers;
  sr_c->cfg->user_headers =  &first_user_header ;

  first_user_header.key = strdup( "newname" );
  first_user_header.value = strdup( newname );

  sr_post( sr_c,  oldname, S_ISREG(sb.st_mode)?(&sb):NULL );
  
  free(first_user_header.key);  
  free(first_user_header.value);  
  first_user_header.key = strdup( "oldname" );
  first_user_header.value = strdup( oldname );

  sr_post( sr_c,  newname, &sb );

  free(first_user_header.key);  
  sr_c->cfg->user_headers = first_user_header.next ;
  
}


int sr_post_cleanup( struct sr_context *sr_c )
{
    amqp_rpc_reply_t reply;

    amqp_exchange_delete( sr_c->cfg->post_broker->conn, 1, amqp_cstring_bytes(sr_c->cfg->exchange), 0 );

    reply = amqp_get_rpc_reply(sr_c->cfg->post_broker->conn);
    if (reply.reply_type != AMQP_RESPONSE_NORMAL ) 
    {
        sr_amqp_reply_print(reply, "failed AMQP get_rpc_reply exchange delete");
        return(0);
    }
    return(1);
}

int sr_post_init( struct sr_context *sr_c )
{
    amqp_rpc_reply_t reply;

    log_msg( LOG_INFO, "declaring exchange %s\n", sr_broker_uri( sr_c->cfg->post_broker )  );

    amqp_exchange_declare( sr_c->cfg->post_broker->conn, 1, amqp_cstring_bytes(sr_c->cfg->post_broker->exchange),
          amqp_cstring_bytes("topic"), 0, sr_c->cfg->durable, 0, 0, amqp_empty_table );

 
    reply = amqp_get_rpc_reply(sr_c->cfg->post_broker->conn);
    if (reply.reply_type != AMQP_RESPONSE_NORMAL ) 
    {
        sr_amqp_reply_print(reply, "failed AMQP get_rpc_reply exchange declare");
        //return(0);
    }

    return(1);
}

