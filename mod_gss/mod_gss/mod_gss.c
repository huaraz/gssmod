/*
 * mod_gss - an RFC2228 GSSAPI module for ProFTPD
 *
 * Copyright (c) 2002-2005 M Moeller <markus_moeller@compuserve.com>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, M Moeller gives permission to link this program
 * with MIT, Heimdal or other GSS/Kerberos libraries, and distribute
 * the resulting executable, without including the source code for
 * for the Libraries in the source distribution.
 *
 *  --- DO NOT DELETE BELOW THIS LINE ----
 *  $Libraries: |GSS_LIBS|$
 *
 *  $MIT-Libraries: -lgssapi_krb5 -ldes425 -lkrb5 -lkcrypto -lcom_err$
 *  $HEIMDAL-Libraries: -lgssapi -lkrb5 -lcom_err -lasn1 -lroken$
 *  $SEAM-Libraries: -lgss -R/usr/lib/gss/(gl/) /usr/lib/gss/(gl/)mech_krb5.so$
 *  $NAS-Libraries: -L/usr/lib -lgssapi_krb5 -lkrb5$
 * 
 * $Id$
 * $Source$
 * $Author$
 * $Date$
 */

/* 
   The following new optional commands are introduced in this
   specification:

      AUTH (Authentication/Security Mechanism),
      ADAT (Authentication/Security Data),
      PROT (Data Channel Protection Level),
      PBSZ (Protection Buffer Size),
      CCC (Clear Command Channel),
      MIC (Integrity Protected Command),
      CONF (Confidentiality Protected Command), and
      ENC (Privacy Protected Command).

   A new class of reply types (6yz) is also introduced for protected
   replies.

   Requires MIT Kerberos libraries.
   Tested against MIT Kerberos ftp client
*/

#include "mod_gss.h"

module gss_module;

/* Module variables */
static char *radixN = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *gss_services[] = { "ftp", "host", 0 };
static char *gss_keytab_file = NULL;
static char *keytab_file = NULL;
static unsigned char gss_engine = FALSE;
static unsigned long gss_flags = 0UL, gss_prot_flags = 0UL, gss_opts = 0UL;
static int           gss_logfd = -1;
static char          *gss_logname = NULL;
static unsigned char gss_required_on_ctrl = FALSE;
static unsigned char gss_required_on_data = FALSE;
static gss_ctx_id_t  gcontext=GSS_C_NO_CONTEXT;
static unsigned int  maxbuf, actualbuf;
static unsigned char *ucbuf;
static gss_buffer_desc client_name,server_name;
   
/* mod_gss session flags */
#define GSS_SESS_AUTH_OK		0x0001
#define GSS_SESS_ADAT_OK		0x0002
#define GSS_SESS_PBSZ_OK		0x0004
#define GSS_SESS_DATA_OPEN		0x0010
#define GSS_SESS_DATA_ERROR		0x0020
#define GSS_SESS_DATA_READ_ERROR	0x0040
#define GSS_SESS_DISPATCH		0x0100
#define GSS_SESS_CCC			0x0200
#define GSS_SESS_FWCCC			0x0400
#define GSS_SESS_CONF_SUP		0x1000

#define GSS_SESS_PROT_C                 0x0000
#define GSS_SESS_PROT_S                 0x0001
#define GSS_SESS_PROT_P                 0x0002
#define GSS_SESS_PROT_E                 0x0004

/* mod_gss option flags*/
#define GSS_OPT_ALLOW_CCC               0x0001
#define GSS_OPT_ALLOW_FW_CCC            0x0002
#define GSS_OPT_ALLOW_FW_NAT		0x0004

static pr_netio_t *gss_data_netio = NULL;
static pr_netio_stream_t *gss_data_rd_nstrm = NULL;
static pr_netio_stream_t *gss_data_wr_nstrm = NULL;

/* GSSAPI support functions */
static char *gss_format_cb(pool *,const char *fmt, ...);
static void gss_closelog(void);
static char *radix_error(int e);
static int  radix_encode(unsigned char inbuf[], unsigned char outbuf[], int *len, int decode);
static void reply_gss_error(char *code, OM_uint32 maj_stat, OM_uint32 min_stat, char *s);
static void log_gss_error(OM_uint32 maj_stat, OM_uint32 min_stat, char *s);
static ssize_t looping_read(int fd, register char *buf,register int len);
static ssize_t looping_write(int fd, register const char *buf, int len);
static int gss_dispatch(char *buf);
static int kpass(char *name, char *pass);

static int gss_openlog(server_rec *s);

static int gss_log(const char *, ...)
#ifdef __GNUC__
__attribute__ ((format (printf, 1, 2)));
#else
;
#endif

/* Support routines for data channel Netio function
 */
/*
   When data transfers are protected between the client and server (in
   either direction), certain transformations and encapsulations must be
   performed so that the recipient can properly decode the transmitted
   file.

   The sender must apply all protection services after transformations
   associated with the representation type, file structure, and transfer
   mode have been performed.  The data sent over the data channel is,
   for the purposes of protection, to be treated as a byte stream.

   When performing a data transfer in an authenticated manner, the
   authentication checks are performed on individual blocks of the file,
   rather than on the file as a whole. Consequently, it is possible for
   insertion attacks to insert blocks into the data stream (i.e.,
   replays) that authenticate correctly, but result in a corrupted file
   being undetected by the receiver. To guard against such attacks, the
   specific security mechanism employed should include mechanisms to
   protect against such attacks.  Many GSS-API mechanisms usable with
   the specification in Appendix I, and the Kerberos mechanism in
   Appendix II do so.

   The sender must take the input byte stream, and break it up into
   blocks such that each block, when encoded using a security mechanism
   specific procedure, will be no larger than the buffer size negotiated
   by the client with the PBSZ command.  Each block must be encoded,
   then transmitted with the length of the encoded block prepended as a
   four byte unsigned integer, most significant byte first.

   When the end of the file is reached, the sender must encode a block
   of zero bytes, and send this final block to the recipient before
   closing the data connection.

   The recipient will read the four byte length, read a block of data
   that many bytes long, then decode and verify this block with a
   security mechanism specific procedure.  This must be repeated until a
   block encoding a buffer of zero bytes is received.  This indicates
   the end of the encoded byte stream.

   Any transformations associated with the representation type, file
   structure, and transfer mode are to be performed by the recipient on
   the byte stream resulting from the above process.

   When using block transfer mode, the sender's (cleartext) buffer size
   is independent of the block size.

   The server will reply 534 to a STOR, STOU, RETR, LIST, NLST, or APPE
   command if the current protection level is not at the level dictated
   by the server's security requirements for the particular file
   transfer.

   If any data protection services fail at any time during data transfer
   at the server end (including an attempt to send a buffer size greater
   than the negotiated maximum), the server will send a 535 reply to the
   data transfer command (either STOR, STOU, RETR, LIST, NLST, or APPE).
*/
/* Appendix I

   The procedure associated with MIC commands, 631 replies, and Safe
   file transfers is:

      GSS_Wrap for the sender, with conf_flag == FALSE

      GSS_Unwrap for the receiver

   The procedure associated with ENC commands, 632 replies, and Private
   file transfers is:

      GSS_Wrap for the sender, with conf_flag == TRUE
      GSS_Unwrap for the receiver

   CONF commands and 633 replies are not supported.

   Both the client and server should inspect the value of conf_avail to
   determine whether the peer supports confidentiality services.

 */
static ssize_t gss_write(pr_netio_stream_t *nstrm, char *buf,int buflen) {
    ssize_t count;
    int  conf_state;
    char *enc_buf;
    pool *pool;
    gss_int32 length;
    gss_uint32 net_len;

    gss_buffer_desc gss_in_buf, gss_out_buf;
    OM_uint32       maj_stat, min_stat;

    gss_in_buf.value = buf;
    gss_in_buf.length = buflen;
      
    maj_stat = gss_wrap(&min_stat, gcontext,
			gss_prot_flags & GSS_SESS_PROT_P , /* confidential */
			GSS_C_QOP_DEFAULT,
			&gss_in_buf, &conf_state,
			&gss_out_buf);

    if (maj_stat != GSS_S_COMPLETE) {
	reply_gss_error(R_535,maj_stat,min_stat,"Could not protect data");
	gss_release_buffer(&min_stat,&gss_out_buf);
	return -1;
    }  else if ((gss_prot_flags & GSS_SESS_PROT_P) && !conf_state) {
	reply_gss_error(R_535,maj_stat,min_stat,"Did not protect data");
	gss_release_buffer(&min_stat, &gss_out_buf);
	return -1;
    } 

    pool = make_sub_pool(session.pool ? session.pool : permanent_pool);
    enc_buf=pcalloc(pool, gss_out_buf.length);
 
    memcpy(enc_buf, gss_out_buf.value, length=gss_out_buf.length);
    gss_release_buffer(&min_stat, &gss_out_buf);

    net_len = htonl((u_long) length);
    if ( (count=looping_write(nstrm->strm_fd,(char *) &net_len, 4)) != 4 ) {
        pr_response_add_err(R_535, "Could not write PROT buffer length %d/%s",
			 (int) count, count == -1 ? strerror(errno):"premature EOF");
        gss_log("GSSAPI Could not write PROT buffer length %d/%s",
                (int) count, count == -1 ? strerror(errno):"premature EOF");
        if (pool)
            destroy_pool(pool);
        return -1;
    }
    if ( (count=looping_write(nstrm->strm_fd, enc_buf,length)) != length) {
        pr_response_add_err(R_535, "Could not write %u byte PROT buffer: %s",
			 length, count == -1 ? strerror(errno) : "premature EOF");
	gss_log("GSSAPI Could not write %u byte PROT buffer: %s",
		length, count == -1 ? strerror(errno) : "premature EOF");
        if (pool)
           destroy_pool(pool);
        return -1;  
    }
    
    if (pool)
       destroy_pool(pool);
    return buflen;
}

static int gss_netio_write_cb(pr_netio_stream_t *nstrm, char *buf,size_t buflen) {

    int     count=0;
    int     total_count=0;        
    char    *p;
 
    OM_uint32   maj_stat, min_stat;
    OM_uint32   max_buf_size;

    if (gss_required_on_data && ! gss_prot_flags) {
        char *mesg="GSS protection required on data channel";

	/* Only accept this if GSS is not required, by policy, on data
	 * connections.
	 */
	pr_response_add_err(R_550, "%s", mesg);
	gss_log("GSSAPI %s", mesg);
        gss_flags |= GSS_SESS_DATA_ERROR;
        return -1;
    }

    if ( ! gss_prot_flags ) {
	return looping_write(nstrm->strm_fd,buf,buflen);
    }
    /*
    Check if protected buffer length will be less then maxbuf !!
    */
    maj_stat = gss_wrap_size_limit(&min_stat,gcontext,
                                   gss_prot_flags & GSS_SESS_PROT_P,
                                   GSS_C_QOP_DEFAULT,
                                   maxbuf,
                                   &max_buf_size);
    if (maj_stat != GSS_S_COMPLETE) {
	reply_gss_error(R_535,maj_stat,min_stat,"Could not determine max wrap size");
	gss_release_buffer(&min_stat,GSS_C_NO_BUFFER);
        gss_flags |= GSS_SESS_DATA_ERROR;
	return -1;
    }
    /* max_buf_size = maximal input buffer size */
    p=buf;
    while ( buflen > total_count ) { 
	/* */ 
	if ( buflen - total_count > max_buf_size ) {
	    if ((count = gss_write(nstrm,p,max_buf_size)) != max_buf_size )
		return -1;
        } else {
	    if ((count = gss_write(nstrm,p,buflen-total_count)) != buflen-total_count )
   	        return -1;
	}       
        total_count = buflen - total_count > max_buf_size ? total_count + max_buf_size : buflen;
        p=p+total_count;
    }

    return buflen;  
}

static int gss_netio_read_cb(pr_netio_stream_t *nstrm, char *buf,
			     size_t buflen) {
    int count;
    int conf_state;
 
    static int msg_count=0;
    static int msg_length=0;
    static char *msg_p;
    static char *dec_buf;
    static pool *pool=NULL;
     
    gss_uint32      length;
    gss_buffer_desc xmit_buf, msg_buf;
    OM_uint32       maj_stat, min_stat;

    if (gss_required_on_data && ! gss_prot_flags) {
        char *mesg="GSS protection required on data channel";

	/* Only accept this if GSS is not required, by policy, on data
	 * connections.
	 */
/*
	pr_response_add_err(R_550, "%s", mesg);
*/
	gss_log("GSSAPI %s", mesg);
        return -1;
    }

    if ( ! gss_prot_flags ) {
        return looping_read(nstrm->strm_fd, buf, buflen);
    }

    /* after successful ADAT and PBSZ/PROT commands we read protected data channel 
       1) read in whole encoded block until ucbuf is full 
       2) decrypt block
       3) return decrypted blocks of maximal buflen length
    */
    if ( msg_length == 0 ) {

        msg_count = 0;
        msg_p = NULL;
        if (pool) {
            destroy_pool(pool);
            pool=NULL;
        }
        dec_buf = NULL;
        /* read length of encoded block */
	count = looping_read(nstrm->strm_fd, (char *)&length, sizeof(length));    
	if ( count != sizeof(length) ){
	    gss_log("GSSAPI Could not read PROT buffer length %d/%s",
		    count, count == -1 ? strerror(errno):"premature EOF");
	    pr_response_add_err(R_535,"Could not read PROT buffer length %d/%s",
			     count, count == -1 ? strerror(errno):"premature EOF");
	    return -1;
	}
	if ((length = (u_long) ntohl(length)) > maxbuf) {
	    gss_log("GSSAPI Length (%d) of PROT buffer > PBSZ=%u",
		    length, maxbuf);
	    pr_response_add_err(R_535,"Length (%d) of PROT buffer > PBSZ=%u",
			     length, maxbuf);
	    return -1;
	}

        /* read encoded block into ucbuf */
        count=looping_read(nstrm->strm_fd,ucbuf,length);

	if ( count != length ) {
	    gss_log("GSSAPI Could not read %u byte PROT buffer: %s",
		    length, count == -1 ? strerror(errno) : "premature EOF");
	    pr_response_add_err(R_535,"Could not read %u byte PROT buffer: %s",
			     length, count == -1 ? strerror(errno) : "premature EOF");
	    return -1;
	}

	xmit_buf.value = ucbuf;
	xmit_buf.length = length;
	conf_state = gss_prot_flags & GSS_SESS_PROT_P;

	/* decrypt/verify the encoded block */
	maj_stat = gss_unwrap(&min_stat, gcontext, &xmit_buf,
			      &msg_buf, &conf_state, NULL);

	if (maj_stat != GSS_S_COMPLETE) {
	    reply_gss_error(R_535, maj_stat, min_stat,
			    gss_prot_flags & GSS_SESS_PROT_P ?
			    "Failed unsealing/unwraping privat message":
			    "Failed unsealing/unwraping safe message");
        
	    return -1;
	}
        if (  msg_buf.length == 0 ){
            /* last encoded empty block */
    	    gss_release_buffer(&min_stat, &msg_buf);
            gss_flags &= ~GSS_SESS_DATA_READ_ERROR;
	    return 0;
        }
        /* copy decrypted message to dec_buf */
        pool = make_sub_pool(session.pool ? session.pool : permanent_pool);
        dec_buf=pcalloc(pool, msg_buf.length);

	memcpy(dec_buf,msg_buf.value,msg_buf.length); 
	msg_length = msg_buf.length;
	msg_p = dec_buf;
        gss_release_buffer(&min_stat, &msg_buf);           
    } 

    /* msg_p points to start of protected message blocks of maximal buflen length */
    msg_count = buflen < msg_length ? buflen : msg_length;
    memcpy(buf, msg_p, msg_count);
    msg_p=msg_p+msg_count;
    msg_length = msg_length - msg_count;
    
    return msg_count;
}

/* NetIO callbacks for data channel
 */

static void gss_netio_abort_cb(pr_netio_stream_t *nstrm) {
    nstrm->strm_flags |= PR_NETIO_SESS_ABORT;
    if ( nstrm->strm_type == PR_NETIO_STRM_DATA && (gss_flags & GSS_SESS_DATA_OPEN) ) {
    	gss_flags |= GSS_SESS_DATA_ERROR;
    }
}

static int gss_netio_shutdown_cb(pr_netio_stream_t *nstrm, int how) {
    ssize_t count=0; 

    if ( nstrm->strm_type == PR_NETIO_STRM_DATA && (gss_flags & GSS_SESS_DATA_OPEN) && gss_prot_flags )  { 
        if ( ! (gss_flags & GSS_SESS_DATA_ERROR) )  { 
            if (nstrm->strm_mode == PR_NETIO_IO_WR) {
            	count = gss_write(nstrm,"",0);
            	if ( count ) 
                    gss_log("GSSAPI Could not write end of protection stream");
	    } else if ( gss_flags & GSS_SESS_DATA_READ_ERROR) { 
                gss_log("GSSAPI Could not read end of protection stream");
	    }
        } else {
            if (nstrm->strm_mode == PR_NETIO_IO_WR)
                gss_log("GSSAPI ABORT Could not write end of protection stream");
            else 
                gss_log("GSSAPI ABORT Could not read end of protection stream");
        }
        gss_flags &= ~GSS_SESS_DATA_OPEN;
    }

    return shutdown(nstrm->strm_fd, how);
}

static int gss_netio_close_cb(pr_netio_stream_t *nstrm) {
    int res = 0;

    if (nstrm->strm_data) {

	if (nstrm->strm_type == PR_NETIO_STRM_DATA) 
	    gss_data_rd_nstrm->strm_data = gss_data_wr_nstrm->strm_data =
		nstrm->strm_data = NULL;
    }

    res = close(nstrm->strm_fd);
    nstrm->strm_fd = -1;

    return res;
}

static pr_netio_stream_t *gss_netio_open_cb(pr_netio_stream_t *nstrm, int fd,
					    int mode) {
    nstrm->strm_fd = fd;
    nstrm->strm_mode = mode;

   /* Cache a pointer to this stream. */
    if (nstrm->strm_type == PR_NETIO_STRM_DATA) {
	if (nstrm->strm_mode == PR_NETIO_IO_RD) {
	    gss_data_rd_nstrm = nstrm;
            gss_flags |= GSS_SESS_DATA_READ_ERROR;
        }

	if (nstrm->strm_mode == PR_NETIO_IO_WR) {
	    gss_data_wr_nstrm = nstrm;
            gss_flags |= GSS_SESS_DATA_OPEN;
            gss_flags &= ~GSS_SESS_DATA_ERROR;
        }
    }

    return nstrm;
}

static int gss_netio_poll_cb(pr_netio_stream_t *nstrm) {
    fd_set rfds, wfds;
    struct timeval tval;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);

    if (nstrm->strm_mode == PR_NETIO_IO_RD)
	FD_SET(nstrm->strm_fd, &rfds);
    else
	FD_SET(nstrm->strm_fd, &wfds);

    tval.tv_sec = (nstrm->strm_flags & PR_NETIO_SESS_INTR) ?
	nstrm->strm_interval : 10;
    tval.tv_usec = 0;

    return select(nstrm->strm_fd + 1, &rfds, &wfds, NULL, &tval);
}

static int gss_netio_postopen_cb(pr_netio_stream_t *nstrm) {
    return 0;
}

static pr_netio_stream_t *gss_netio_reopen_cb(pr_netio_stream_t *nstrm, int fd,
					      int mode) {

    if (nstrm->strm_fd != -1)
	close(nstrm->strm_fd);

    nstrm->strm_fd = fd;
    nstrm->strm_mode = mode;

    /* NOTE: a no-op? */
    return nstrm;
}

static void gss_netio_install_data(void) {
    pr_netio_t *netio = gss_data_netio ? gss_data_netio :
	(gss_data_netio = pr_alloc_netio(session.pool ? session.pool :
					 permanent_pool));

    netio->abort = gss_netio_abort_cb;
    netio->close = gss_netio_close_cb;
    netio->shutdown = gss_netio_shutdown_cb;
    netio->open = gss_netio_open_cb;
    netio->poll = gss_netio_poll_cb;
    netio->postopen = gss_netio_postopen_cb;
    netio->read = gss_netio_read_cb;
    netio->reopen = gss_netio_reopen_cb;
    netio->write = gss_netio_write_cb;

    pr_unregister_netio(PR_NETIO_STRM_DATA);

    if (pr_register_netio(netio, PR_NETIO_STRM_DATA) < 0)
	pr_log_pri(LOG_INFO, MOD_GSS_VERSION ": error registering netio: %s",
		strerror(errno));

    /* Install our response protection handler */
    pr_response_register_handler(gss_format_cb);
}

/* Logging functions
 */

static void gss_closelog(void) {

    /* Sanity check */
    if (gss_logfd != -1) {
	close(gss_logfd);
	gss_logfd = -1;
	gss_logname = NULL;
    }

    return;
}

static int gss_log(const char *fmt, ...) {
    char buf[PR_TUNABLE_BUFFER_SIZE] = {'\0'};
    time_t timestamp = time(NULL);
    struct tm *t = NULL;
    va_list msg;

    /* Sanity check */
    if (!gss_logname) {
 
	return 0;
    }

    if (!strcasecmp(gss_logname, "syslog")) {
        va_start(msg, fmt);
        vsnprintf(buf, sizeof(buf), fmt, msg);
        va_end(msg);
        buf[sizeof(buf)-1] = '\0';
        pr_log_pri(PR_LOG_NOTICE,buf);
	return 0;
    }
	  

    t = localtime(&timestamp);

    /* Prepend the timestamp */
    strftime(buf, sizeof(buf), "%b %d %H:%M:%S ", t);
    buf[sizeof(buf)-1] = '\0';

    /* Prepend a small header */
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), MOD_GSS_VERSION
	     "[%u]: ", (unsigned int) getpid());
    buf[sizeof(buf)-1] = '\0';

    /* Affix the message */
    va_start(msg, fmt);
    vsnprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), fmt, msg);
    va_end(msg);

    buf[strlen(buf)] = '\n';
    buf[sizeof(buf)-1] = '\0';

    if (write(gss_logfd, buf, strlen(buf)) < 0)
	return -1;

    return 0;
}

static int gss_openlog(server_rec *s) {
    int res = 0;

    /* Sanity checks */
    if ( ! s ) {
        if ((gss_logname = get_param_ptr(main_server->conf, "GSSLog",
				     FALSE)) == NULL)
	    return 0;
    } else {
        if ((gss_logname = get_param_ptr(s->conf, "GSSLog",
				     FALSE)) == NULL)
	    return 0;
    }

    if (!strcasecmp(gss_logname, "none")) {
	gss_logname = NULL;
	return 0;
    }

    if (!strcasecmp(gss_logname, "syslog")) {
	return 0;
    }

    pr_alarms_block();
    PRIVS_ROOT
	res = pr_log_openfile(gss_logname, &gss_logfd, 0600);
    PRIVS_RELINQUISH
	pr_alarms_unblock();

    return res;
}

/* Authentication handlers
*/
/*
   The security data exchange may, among other things, establish the
   identity of the client in a secure way to the server.  This identity
   may be used as one input to the login authorization process.

   In response to the FTP login commands (AUTH, PASS, ACCT), the server
   may choose to change the sequence of commands and replies specified
   by RFC 959 as follows.  There are also some new replies available.

   If the server is willing to allow the user named by the USER command
   to log in based on the identity established by the security data
   exchange, it should respond with reply code 232.

   If the security mechanism requires a challenge/response password, it
   should respond to the USER command with reply code 336.  The text
   part of the reply should contain the challenge.  The client must
   display the challenge to the user before prompting for the password
   in this case.  This is particularly relevant to more sophisticated
   clients or graphical user interfaces which provide dialog boxes or
   other modal input.  These clients should be careful not to prompt for
   the password before the username has been sent to the server, in case
   the user needs the challenge in the 336 reply to construct a valid
   password.
 */
/* Appendix I

   Since GSSAPI supports anonymous peers to security contexts, it is
   possible that the client's authentication of the server does not
   actually establish an identity.

   When the security state is reset (when AUTH is received a second
   time, or when REIN is received), this should be done by calling the
   GSS_Delete_sec_context function.

 */
/* This function does the main authentication work, and is called in the
 * normal course of events:
 *
 *   cmd->argv[0]: user name
 *   cmd->argv[1]: cleartext password
 */
MODRET gss_authenticate(cmd_rec *cmd) {
    krb5_boolean k5ret;
    krb5_context kc;
    krb5_principal p;
    krb5_error_code kerr;

    if (!gss_engine)
	return DECLINED(cmd);

    /* only allow Kerberos 5 authentication if GSSAPI was used */
    if ( !(gss_flags & GSS_SESS_AUTH_OK) )
	return DECLINED(cmd);

    kerr = krb5_init_context(&kc);
    if (kerr) {
        gss_log("GSSAPI Could not initialise krb5 context (%s)",error_message(kerr));
        return ERROR_INT(cmd,PR_AUTH_ERROR);
    }
    
    if (!client_name.value) {
        if (gss_flags & GSS_SESS_ADAT_OK ) {
           gss_log("GSSAPI Client name not set, but ADAT successful");
           return ERROR_INT(cmd,PR_AUTH_ERROR);
        } else {
           gss_log("GSSAPI Client name not set and ADAT not successful. Use other methods to authenticate.");       
           return DECLINED(cmd);
	} 
    }

    kerr = krb5_parse_name(kc, client_name.value, &p);
    if (kerr) { 
        gss_log("GSSAPI Could not parse krb5 name (%s).",error_message(kerr));
        krb5_free_context(kc);
        return ERROR_INT(cmd,PR_AUTH_ERROR);
    }

    /* Check first if principal and local user match or .k5login has correct entry,
     * then check password against kdc
     */
    pr_signals_block();
    PRIVS_ROOT
    k5ret = krb5_kuserok(kc, p, cmd->argv[0]);
    PRIVS_RELINQUISH
    pr_signals_unblock();
    krb5_free_principal(kc, p);
    if (k5ret == TRUE) {
       gss_log("GSSAPI User %s is authorized as %s.", (char *) client_name.value,cmd->argv[0]);
       return mod_create_data(cmd, (void *) PR_AUTH_RFC2228_OK);
    } else {

	/* check password against kdc */
	if (kpass(cmd->argv[0],cmd->argv[1])) {
            gss_log("GSSAPI User %s is not authorized as %s. Use other methods to authenticate.", (char *) client_name.value,cmd->argv[0]);
            return DECLINED(cmd);
        } else {
           gss_log("GSSAPI User %s/%s authorized by kdc.",cmd->argv[0],client_name.value? (char *) client_name.value:"-"); 
           return mod_create_data(cmd, (void *) PR_AUTH_RFC2228_OK);
        }
    }
}
/* This function does the main authentication work, and is called in the
 * normal course of events:
 *
 *   cmd->argv[0]: hashed password
 *   cmd->argv[1]: user name
 *   cmd->argv[2]: cleartext password
 */
MODRET gss_auth_check(cmd_rec *cmd) {
    krb5_boolean k5ret;
    krb5_context kc;
    krb5_principal p;
    krb5_error_code kerr;

    if (!gss_engine)
	return DECLINED(cmd);

    /* only allow Kerberos 5 authentication if GSSAPI was used */
    if ( !(gss_flags & GSS_SESS_AUTH_OK) )
	return DECLINED(cmd);
    

    kerr = krb5_init_context(&kc);
    if (kerr) {
        gss_log("GSSAPI Could not initialise krb5 context (%s).",error_message(kerr));
        return ERROR_INT(cmd,PR_AUTH_ERROR);
    }

    if (!client_name.value) {
        if (gss_flags & GSS_SESS_ADAT_OK ) {
           gss_log("GSSAPI Client name not set, but ADAT successful");
	   return ERROR_INT(cmd,PR_AUTH_ERROR);
        } else {
           gss_log("GSSAPI Client name not set and ADAT not successful. Use other methods to authenticate.");
           return DECLINED(cmd);
	}
    }

    kerr = krb5_parse_name(kc, client_name.value, &p);
    if (kerr) { 
        gss_log("GSSAPI Could not parse krb5 name (%s).",error_message(kerr));
        krb5_free_context(kc);
        return ERROR_INT(cmd,PR_AUTH_ERROR);
    }

    /* Check first if principal and local user match or .k5login has correct entry,
     * then check password against kdc
     */
    k5ret = krb5_kuserok(kc, p, cmd->argv[1]);
    krb5_free_principal(kc, p);
    if (k5ret == TRUE) {
       gss_log("GSSAPI User %s is authorized as %s.", (char *) client_name.value,cmd->argv[1]);
       return mod_create_data(cmd, (void *) PR_AUTH_RFC2228_OK);
    } else {
	/* check password against kdc */
	if (kpass(cmd->argv[1],cmd->argv[2])) {
            gss_log("GSSAPI User %s is not authorized as %s. Use other methods to authenticate.", (char *) client_name.value,cmd->argv[1]);
            return DECLINED(cmd);
        } else  {
           gss_log("GSSAPI User %s/%s authorized by kdc.",cmd->argv[1],client_name.value? (char *) client_name.value:"-");            
           return mod_create_data(cmd, (void *) PR_AUTH_RFC2228_OK);
        }
    }
}


/* Command handlers
 */
MODRET gss_ccc(cmd_rec *cmd) {
    char *mesg = "CCC (clear command channel) not supported.";

    if (!gss_engine)
	return DECLINED(cmd);

    CHECK_CMD_ARGS(cmd, 1);

    if ( session.rfc2228_mech && strcmp(session.rfc2228_mech, "GSSAPI") != 0 ) {
        return DECLINED(cmd);
    } else if (!(gss_flags & GSS_SESS_ADAT_OK)) {
        pr_response_add_err(R_503, "Security data exchange not completed");
        gss_log("GSSAPI security data exchange not completed before %s command",cmd->argv[0]);
        return ERROR(cmd);
    }
 
    if (gss_opts & GSS_OPT_ALLOW_CCC){
        gss_flags |= GSS_SESS_CCC;
        pr_response_add(R_200, "CCC command successful");
        return HANDLED(cmd);
    }

    pr_response_add_err(R_534, mesg);
    gss_log("GSSAPI %s", mesg);
    return ERROR(cmd);

}

MODRET gss_fwccc(cmd_rec *cmd) {
    char *mesg = "FWCCC (clear PORT/EPORT/PASV/EPASV command) not supported.";

    if (!gss_engine)
        return DECLINED(cmd);

    CHECK_CMD_ARGS(cmd, 1);

    if ( session.rfc2228_mech && strcmp(session.rfc2228_mech, "GSSAPI") != 0 ) {
        return DECLINED(cmd);
    } else if (!(gss_flags & GSS_SESS_ADAT_OK)) {
        pr_response_add_err(R_503, "Security data exchange not completed");
        gss_log("GSSAPI security data exchange not completed before %s command",cmd->argv[0]);
        return ERROR(cmd);
    }

    if (gss_opts & GSS_OPT_ALLOW_FW_CCC){
        if ( gss_flags & GSS_SESS_FWCCC )
           gss_flags &= ~GSS_SESS_FWCCC;
        else
           gss_flags |= GSS_SESS_FWCCC;
        pr_response_add(R_200, "FWCCC command successfully switched %s",gss_flags & GSS_SESS_FWCCC ? "On":"Off");
        return HANDLED(cmd);
    }

    pr_response_add_err(R_534, mesg);
    gss_log("GSSAPI %s", mesg);
    return ERROR(cmd);

}

/* Appendix I

   The procedure associated with MIC commands, 631 replies, and Safe
   file transfers is:

      GSS_Wrap for the sender, with conf_flag == FALSE

      GSS_Unwrap for the receiver

   The procedure associated with ENC commands, 632 replies, and Private
   file transfers is:

      GSS_Wrap for the sender, with conf_flag == TRUE
      GSS_Unwrap for the receiver

   CONF commands and 633 replies are not supported.

   Both the client and server should inspect the value of conf_avail to
   determine whether the peer supports confidentiality services.

 */
MODRET gss_dec(cmd_rec *cmd) {
    int  error, i;
    int  conf_state=1;
    char *dec_buf=NULL;

    gss_buffer_desc xmit_buf, msg_buf;
    OM_uint32       maj_stat, min_stat;

    if (!gss_engine)
	return DECLINED(cmd);

    CHECK_CMD_ARGS(cmd, 2);

    if ( session.rfc2228_mech && strcmp(session.rfc2228_mech, "GSSAPI") != 0 ) {
        return DECLINED(cmd);
    } else if (!(gss_flags & GSS_SESS_ADAT_OK)) {
        pr_response_add_err(R_503, "Security data exchange not completed");
        gss_log("GSSAPI security data exchange not completed before %s command",cmd->argv[0]);
        return ERROR(cmd);
    }

    if ( ! strcmp(cmd->argv[0], "CONF") ) {
        pr_response_add_err(R_537,"CONF protection level not supported.");
        gss_log("GSSAPI CONF protection level not supported.");
/*
  session.sp_flags = SP_CONF;
*/
        return ERROR(cmd);
    } else if ( ! strcmp(cmd->argv[0], "ENC") && (gss_flags & GSS_SESS_CONF_SUP)) { 
        session.sp_flags = SP_ENC;
	conf_state = 1;
    } else if ( ! strcmp(cmd->argv[0], "MIC") ) {
       session.sp_flags = SP_MIC;
       conf_state = 0;
    } else {
        pr_response_add_err(R_536,"Protection level %s not supported.",cmd->argv[0]);
        gss_log("GSSAPI Protection level %s not supported.",cmd->argv[0]);
        return ERROR(cmd);
    }

    /* remove trailing \r \n */
    for ( i=strlen(cmd->arg) ; i>=0 ; i-- ) {
	if ( cmd->arg[i] == '\r' || cmd->arg[i] == '\n' ) cmd->arg[i] = '\0';
    } 

    xmit_buf.length=strlen(cmd->arg)+1;
    /* protected string <= unprotected string */
    xmit_buf.value=pcalloc(cmd->pool,strlen(cmd->arg)+1);
    if ((error = radix_encode(cmd->arg, xmit_buf.value, (int *)&xmit_buf.length, 1)) != 0 ) {
	pr_response_add_err(R_501, "Couldn't base 64 decode argument to %s command (%s)",
		session.sp_flags & SP_ENC ? "ENC" : 
                session.sp_flags & SP_MIC ? "MIC" : "", radix_error(error));
	gss_log("GSSAPI Can't base 64 decode argument to %s command (%s)",
		session.sp_flags & SP_ENC ? "ENC" : 
                session.sp_flags & SP_MIC ? "MIC" : "", radix_error(error));
        return ERROR(cmd);
    }

    /* decrypt the message */
    maj_stat = gss_unwrap(&min_stat, gcontext, &xmit_buf,
			  &msg_buf, &conf_state, NULL);

    if (maj_stat != GSS_S_COMPLETE) {
	log_gss_error( maj_stat, min_stat, 
                       session.sp_flags & SP_ENC ? 
                       "failed unsealing/unwraping ENC message" :
                       session.sp_flags & SP_MIC ? 
		       "failed unsealing/unwraping MIC message" :
                       "failed unsealing/unwraping message");
        gss_release_buffer(&min_stat,&msg_buf);
	pr_response_add_err(R_535, session.sp_flags & SP_ENC ? 
                       "failed unsealing/unwraping ENC message" :
                       session.sp_flags & SP_MIC ? 
		       "failed unsealing/unwraping MIC message":
                       "failed unsealing/unwraping message");
        return ERROR(cmd);
    }
    
    dec_buf=pcalloc(cmd->pool, msg_buf.length+1);
    memcpy(dec_buf,msg_buf.value,msg_buf.length); 
    dec_buf[msg_buf.length]='\0';
    /* remove trailing \r \n */
    for ( i=strlen(dec_buf) ; i>=0 ; i-- ) {
        if ( dec_buf[i] == '\r' || dec_buf[i] == '\n' ) dec_buf[i] = '\0';
    }

    if (strncmp("PASS ", dec_buf, 5) == 0)
        pr_log_debug(DEBUG9,"GSSAPI unwrapped command 'PASS (hidden)'");
    else
        pr_log_debug(DEBUG9,"GSSAPI unwrapped command '%s'",dec_buf);
    gss_release_buffer(&min_stat,&msg_buf);

    gss_flags |= GSS_SESS_DISPATCH;
    if ( gss_dispatch(dec_buf) ) {
        gss_flags &= ~GSS_SESS_DISPATCH;
      	pr_response_add_err(R_535,"command %s rejected",dec_buf);
        gss_log("GSSAPI Failed dispatching command %s",dec_buf);
        return ERROR(cmd);
    }
    gss_flags &= ~GSS_SESS_DISPATCH;

    return HANDLED(cmd);
}

MODRET gss_any(cmd_rec *cmd) {

    if (!gss_engine)
	return DECLINED(cmd);

    /* from mod_tls:
     *
     * NOTE: possibly add checks of commands here in order to support the
     * ability of having GSSRequired in per-directory configurations.  This
     * would mean watching for directory change commands, file transfer
     * commands, and doing a context check in order to appropriately set
     * the value of gss_required_on_data.
     */

    /* Some commands need not be ignored. */
    if (!strcmp(cmd->argv[0], C_AUTH) ||
	!strcmp(cmd->argv[0], C_ADAT) ||
	!strcmp(cmd->argv[0], C_ENC) ||
	!strcmp(cmd->argv[0], C_MIC) ||
	!strcmp(cmd->argv[0], C_CONF))
        return DECLINED(cmd);

    /* Ignore commands from dispatched gss_dec*/
    if (gss_flags & GSS_SESS_DISPATCH) {
        return DECLINED(cmd);
    }

    /* Ignore clear PORT/PASV commands if FWCCC is allowed*/
    if (( (gss_flags & GSS_SESS_FWCCC) && !strcmp(cmd->argv[0], C_PORT) ) ||
        ( (gss_flags & GSS_SESS_FWCCC) && !strcmp(cmd->argv[0], C_PASV) ) ||
        ( (gss_flags & GSS_SESS_FWCCC) && !strcmp(cmd->argv[0], C_EPRT) ) ||
        ( (gss_flags & GSS_SESS_FWCCC) && !strcmp(cmd->argv[0], C_EPSV) )) {
        session.sp_flags = SP_CCC; 
        return DECLINED(cmd);
    }

    /* Ignore clear commands if CCC is allowed*/
    if ((gss_opts & GSS_OPT_ALLOW_CCC) && (gss_flags & GSS_SESS_CCC)) {
        session.sp_flags = SP_CCC; 
        return DECLINED(cmd);
    }

    /* protection on control channel is required (except for commands dispatched by gss_dec) */
    if ( gss_required_on_ctrl ) {
	pr_response_add_err(R_550, "GSS protection required on control channel");
	gss_log("GSSAPI GSS protection required on control channel");
	return ERROR(cmd);
    }

    /* After ADAT all commands have to be protected by ENC/MIC/CONF */
    if ( (gss_flags & GSS_SESS_ADAT_OK) ) {
        session.sp_flags = SP_CCC; 
	pr_response_add_err(R_533, "All commands must be protected.");
	gss_log("GSSAPI Unprotected command(%s) received",cmd->argv[0]);
	return ERROR(cmd);
    }

    return DECLINED(cmd);
}

/*
   AUTHENTICATION/SECURITY MECHANISM (AUTH)

      The argument field is a Telnet string identifying a supported
      mechanism.  This string is case-insensitive.  Values must be
      registered with the IANA, except that values beginning with "X-"
      are reserved for local use.

      If the server does not recognize the AUTH command, it must respond
      with reply code 500.  This is intended to encompass the large
      deployed base of non-security-aware ftp servers, which will
      respond with reply code 500 to any unrecognized command.  If the
      server does recognize the AUTH command but does not implement the
      security extensions, it should respond with reply code 502.

      If the server does not understand the named security mechanism, it
      should respond with reply code 504.

      If the server is not willing to accept the named security
      mechanism, it should respond with reply code 534.

      If the server is not able to accept the named security mechanism,
      such as if a required resource is unavailable, it should respond
      with reply code 431.

      If the server is willing to accept the named security mechanism,
      but requires security data, it must respond with reply code 334.

      If the server is willing to accept the named security mechanism,
      and does not require any security data, it must respond with reply
      code 234.

      If the server is responding with a 334 reply code, it may include
      security data as described in the next section.

      Some servers will allow the AUTH command to be reissued in order
      to establish new authentication.  The AUTH command, if accepted,
      removes any state associated with prior FTP Security commands.
      The server must also require that the user reauthorize (that is,
      reissue some or all of the USER, PASS, and ACCT commands) in this
      case (see section 4 for an explanation of "authorize" in this
      context).
*/
/* Appendix I

   The security mechanism name (for the AUTH command) associated with
   all mechanisms employing the GSSAPI is GSSAPI.  If the server
   supports a security mechanism employing the GSSAPI, it must respond
   with a 334 reply code indicating that an ADAT command is expected
   next.

 */
MODRET gss_auth(cmd_rec *cmd) {
    unsigned int i = 0;
    OM_uint32 maj_stat, min_stat;

    if (!gss_engine)
	return DECLINED(cmd);

    CHECK_CMD_ARGS(cmd, 2);

    if (gss_flags & GSS_SESS_AUTH_OK) {
        gss_log("GSSAPI Reinitialize GSSAPI authentication");
        /* Delete of old credentials means the response cannot be protected.
         * Client has to understand cleartest answers.
         * 
         */
        if ( gcontext ) {
            maj_stat=gss_delete_sec_context(&min_stat,&gcontext,GSS_C_NO_BUFFER);
            if (maj_stat != GSS_S_COMPLETE) {
	       log_gss_error( maj_stat, min_stat,"could not delete credential");
               gss_release_buffer(&min_stat,GSS_C_NO_BUFFER);
            }
            gcontext = GSS_C_NO_CONTEXT;
        }
    }
    /* If session.user is set, we had a valid login reinforce USER,PASS,ACCT*/
    if (session.user) {
        end_login(0);
    }
    
    /* Convert the parameter to upper case */
    for (i = 0; i < strlen(cmd->argv[1]); i++)
	(cmd->argv[1])[i] = toupper((cmd->argv[1])[i]);

    if (!strcmp(cmd->argv[1], "GSSAPI")) { 
	pr_response_send(R_334, "Using authentication type %s; ADAT must follow", cmd->argv[1]);
	gss_log("GSSAPI Auth GSSAPI requested, ADAT must follow");
	gss_flags |= GSS_SESS_AUTH_OK;
    } else {
/*
  pr_response_add_err(R_504, "AUTH %s unsupported", cmd->argv[1]);
  return ERROR(cmd);
*/
        gss_flags &= ~GSS_SESS_AUTH_OK;
        return DECLINED(cmd);
    }

    session.rfc2228_mech = "GSSAPI";
    return HANDLED(cmd);
}

/*
   AUTHENTICATION/SECURITY DATA (ADAT)

      The argument field is a Telnet string representing base 64 encoded
      security data (see Section 9, "Base 64 Encoding").  If a reply
      code indicating success is returned, the server may also use a
      string of the form "ADAT=base64data" as the text part of the reply
      if it wishes to convey security data back to the client.

      The data in both cases is specific to the security mechanism
      specified by the previous AUTH command.  The ADAT command, and the
      associated replies, allow the client and server to conduct an
      arbitrary security protocol.  The security data exchange must
      include enough information for both peers to be aware of which
      optional features are available.  For example, if the client does
      not support data encryption, the server must be made aware of
      this, so it will know not to send encrypted command channel
      replies.  It is strongly recommended that the security mechanism
      provide sequencing on the command channel, to insure that commands
      are not deleted, reordered, or replayed.

      The ADAT command must be preceded by a successful AUTH command,
      and cannot be issued once a security data exchange completes
      (successfully or unsuccessfully), unless it is preceded by an AUTH
      command to reset the security state.

      If the server has not yet received an AUTH command, or if a prior
      security data exchange completed, but the security state has not
      been reset with an AUTH command, it should respond with reply code
      503.

      If the server cannot base 64 decode the argument, it should
      respond with reply code 501.

      If the server rejects the security data (if a checksum fails, for
      instance), it should respond with reply code 535.

      If the server accepts the security data, and requires additional
      data, it should respond with reply code 335.

      If the server accepts the security data, but does not require any
      additional data (i.e., the security data exchange has completed
      successfully), it must respond with reply code 235.

      If the server is responding with a 235 or 335 reply code, then it
      may include security data in the text part of the reply as
      specified above.

      If the ADAT command returns an error, the security data exchange
      will fail, and the client must reset its internal security state.
      If the client becomes unsynchronized with the server (for example,
      the server sends a 234 reply code to an AUTH command, but the
      client has more data to transmit), then the client must reset the
      server's security state.

 */
/* Appendix I

   The client must begin the authentication exchange by calling
   GSS_Init_Sec_Context, passing in 0 for input_context_handle
   (initially), and a targ_name equal to output_name from
   GSS_Import_Name called with input_name_type of Host-Based Service and
   input_name_string of "ftp@hostname" where "hostname" is the fully
   qualified host name of the server with all letters in lower case.
   (Failing this, the client may try again using input_name_string of
   "host@hostname".) The output_token must then be base 64 encoded and
   sent to the server as the argument to an ADAT command.  If
   GSS_Init_Sec_Context returns GSS_S_CONTINUE_NEEDED, then the client
   must expect a token to be returned in the reply to the ADAT command.
   This token must subsequently be passed to another call to
   GSS_Init_Sec_Context.  In this case, if GSS_Init_Sec_Context returns
   no output_token, then the reply code from the server for the previous
   ADAT command must have been 235.  If GSS_Init_Sec_Context returns
   GSS_S_COMPLETE, then no further tokens are expected from the server,
   and the client must consider the server authenticated.

   The server must base 64 decode the argument to the ADAT command and
   pass the resultant token to GSS_Accept_Sec_Context as input_token,
   setting acceptor_cred_handle to NULL (for "use default credentials"),
   and 0 for input_context_handle (initially).  If an output_token is
   returned, it must be base 64 encoded and returned to the client by
   including "ADAT=base64string" in the text of the reply.  If
   GSS_Accept_Sec_Context returns GSS_S_COMPLETE, the reply code must be
   235, and the server must consider the client authenticated.  If
   GSS_Accept_Sec_Context returns GSS_S_CONTINUE_NEEDED, the reply code
   must be 335.  Otherwise, the reply code should be 535, and the text
   of the reply should contain a descriptive error message.

   The chan_bindings input to GSS_Init_Sec_Context and
   GSS_Accept_Sec_Context should use the client internet address and
   server internet address as the initiator and acceptor addresses,
   respectively.  The address type for both should be GSS_C_AF_INET. No
   application data should be specified.
 */
MODRET gss_adat(cmd_rec *cmd) {
    int error = 0;
    int ret_flags = 0;
    int found = 0;
    int replied = 0;

    gss_buffer_desc tok, out_tok, name_buf;
    gss_name_t      server;
    gss_name_t      client;
    gss_cred_id_t   server_creds;
    gss_OID         mechid;
    OM_uint32 acquire_maj=0, acquire_min;
    OM_uint32 accept_maj=0, accept_min;
    OM_uint32 maj_stat=0, min_stat;

    char localname[MAXHOSTNAMELEN];
    char service_name[MAXHOSTNAMELEN+10];
    char **service;
    char *gbuf ;
  
    gss_channel_bindings_t chan=GSS_C_NO_CHANNEL_BINDINGS;

    if (!gss_engine)
        return DECLINED(cmd);

    if (!(gss_opts & GSS_OPT_ALLOW_FW_NAT)) { 
        chan = pcalloc(cmd->tmp_pool,sizeof(*chan));
        switch (pr_netaddr_get_family(session.c->remote_addr)) {
#ifdef USE_IPV6
            case AF_INET6:
                chan->initiator_addrtype = GSS_C_AF_INET6;
	        break;
#endif
            case AF_INET:
                chan->initiator_addrtype = GSS_C_AF_INET;
	        break;
            default:
	        gss_log("GSSAPI Unknown remote address family %d",pr_netaddr_get_family(session.c->remote_addr));
        }
        chan->initiator_address.length = pr_netaddr_get_inaddr_len(session.c->remote_addr);
        chan->initiator_address.value = pr_netaddr_get_inaddr(session.c->remote_addr);

        switch (pr_netaddr_get_family(session.c->local_addr)) {
#ifdef USE_IPV6
            case AF_INET6:
                chan->acceptor_addrtype = GSS_C_AF_INET6;
                break;
#endif
            case AF_INET:
                chan->acceptor_addrtype = GSS_C_AF_INET;
                break;
            default:
                gss_log("GSSAPI Unknown local address family %d",pr_netaddr_get_family(session.c->local_addr));
        }
        chan->acceptor_address.length = pr_netaddr_get_inaddr_len(session.c->local_addr);
        chan->acceptor_address.value = pr_netaddr_get_inaddr(session.c->local_addr);

        chan->application_data.length = 0;
        chan->application_data.value = 0;
    } else {
        pr_log_debug(DEBUG9, "GSSAPI Ignore Channel Binding");
	gss_log("GSSAPI Ignore Channel Binding");
    }

    CHECK_CMD_ARGS(cmd, 2);

    if ( session.rfc2228_mech && strcmp(session.rfc2228_mech, "GSSAPI") != 0 ) {
        return DECLINED(cmd);
    } else if (!(gss_flags & GSS_SESS_AUTH_OK)) {
	pr_response_add_err(R_503, "You must issue the AUTH command prior to ADAT");
        gss_log("GSSAPI You must issue the AUTH command prior to ADAT");
        return ERROR(cmd); 
    }
  
    /* Use cmd->arg instead of cmd->argv[1]

    cmd->arg     entire argument (excluding command)
    cmd-argv[i]  i-th argument

    ADAT has only one argument so cmd->arg is the same as cmd->argv[1] except 
    cmd-argv[1] has been overwritten to avoid logging the sensitive argument 
    by the core log commands
    */

    tok.value=pcalloc(cmd->tmp_pool,strlen(cmd->arg));
    if ((error = radix_encode(cmd->arg, tok.value , (int *)&tok.length, 1)) !=0  ) {
	pr_response_add_err(R_501, "Couldn't decode ADAT (%s)",radix_error(error)); 
	gss_log("GSSAPI Could not decode ADAT (%s)",radix_error(error));
	return ERROR(cmd);
    }

    if (strlen(pr_netaddr_get_dnsstr(session.c->local_addr)) > MAXHOSTNAMELEN -1 ) {
	gss_log("GSSAPI Hostname (%s) longer then MAXHOSTNAMELEN:%d",pr_netaddr_get_dnsstr(session.c->local_addr),MAXHOSTNAMELEN);
        pr_response_add_err(R_535, "Internal error"); 
	return ERROR(cmd);
    }
    strcpy(localname,pr_netaddr_get_dnsstr(session.c->local_addr));

    if (gss_keytab_file) {
	char *p;
        gss_log("GSSAPI Set KRB5_KTNAME=FILE:%s",gss_keytab_file);
        if ( (p = getenv("KRB5_KTNAME")) != NULL )
            keytab_file = strdup(p);
        putenv(pstrcat(main_server->pool, "KRB5_KTNAME=FILE:", gss_keytab_file, NULL));
    } else {
        gss_log("GSSAPI Use default KRB5 keytab");
    }

    for (service = gss_services; *service; service++) {
	sprintf(service_name, "%s@%s", *service, localname);
	name_buf.value = service_name;
	name_buf.length = strlen(name_buf.value) + 1;
        gss_log("GSSAPI Importing service <%s>", service_name);
	pr_log_debug(DEBUG1, "GSSAPI Importing <%s>", service_name);
	maj_stat = gss_import_name(&min_stat, &name_buf,
				   gss_nt_service_name,
				   &server);
	if (maj_stat != GSS_S_COMPLETE) {
	    reply_gss_error(R_535, maj_stat, min_stat,"Failed importing name");
            gss_release_buffer(&min_stat,&name_buf);
            goto err;
	}

	acquire_maj = gss_acquire_cred(&acquire_min, server, 0,
				       GSS_C_NULL_OID_SET, GSS_C_ACCEPT,
				       &server_creds, NULL, NULL);

	if (acquire_maj != GSS_S_COMPLETE) {
            log_gss_error(acquire_maj, acquire_min, "could not acquire credential");
	    continue;
	}

	found++;
	gcontext = GSS_C_NO_CONTEXT;
	accept_maj = gss_accept_sec_context(&accept_min,
	   				    &gcontext, /* context_handle */
					    server_creds, /* verifier_cred_handle */
					    &tok, /* input_token */
					    chan, /* channel bindings */
					    &client, /* src_name */
					    &mechid, /* mech_type */
					    &out_tok, /* output_token */
					    &ret_flags,
					    NULL,       /* ignore time_rec */
					    NULL   /* ignore del_cred_handle */
	                                    );
	if (accept_maj == GSS_S_COMPLETE || accept_maj == GSS_S_CONTINUE_NEEDED ) {
	    break;
        } else {
           log_gss_error(accept_maj, accept_min, "did not accept credential");        
	}
    }
    if (found) {
	if (accept_maj != GSS_S_COMPLETE && accept_maj != GSS_S_CONTINUE_NEEDED ) {
	    reply_gss_error(R_535, accept_maj, accept_min,
			    "Failed accepting context");
	    gss_release_buffer(&accept_min, &out_tok);
	    gss_release_cred(&acquire_min, &server_creds);
	    gss_release_name(&min_stat, &server);
            goto err;
	}
    } else {
	reply_gss_error(R_535, acquire_maj, acquire_min,
			"Error acquiring credentials");
	gss_release_buffer(&accept_min, &out_tok);
        gss_release_cred(&acquire_min, &server_creds);
	gss_release_name(&min_stat, &server);
        goto err;
    }

    if (out_tok.length) {
        gbuf = pcalloc(cmd->tmp_pool,out_tok.length*4+1);
	if ( (error = radix_encode(out_tok.value, gbuf, (int *)&out_tok.length, 0)) != 0 ) {
	    pr_response_add_err(R_535,"Couldn't encode ADAT reply (%s)",
			     radix_error(error));
	    gss_log("GSSAPI Could not encode ADAT reply (%s)",radix_error(error));
   	    gss_release_buffer(&accept_min, &out_tok);
            gss_release_cred(&acquire_min, &server_creds);
	    gss_release_name(&min_stat, &server);
            goto err;
	}
	if (accept_maj == GSS_S_COMPLETE) {
	    pr_response_send(R_235, "ADAT=%s", gbuf);
	    gss_flags |= GSS_SESS_ADAT_OK;
	    replied = 1;
	} else {
	    /* If the server accepts the security data, and
	       requires additional data, it should respond with
	       reply code 335. */
	    pr_response_send(R_335, "ADAT=%s", gbuf);
	}
    }
    if (accept_maj == GSS_S_COMPLETE) {
	/* GSSAPI authentication succeeded */
	maj_stat = gss_display_name(&min_stat, client, &client_name,
				    &mechid);
	if (maj_stat != GSS_S_COMPLETE) {
	    /* "If the server rejects the security data (if
	       a checksum fails, for instance), it should
	       respond with reply code 535." */
	    reply_gss_error(R_535, maj_stat, min_stat,
			    "Extracting GSSAPI identity name");
   	    gss_release_buffer(&accept_min, &out_tok);
            gss_release_cred(&acquire_min, &server_creds);
	    gss_release_name(&min_stat, &server);
            goto err;
	}
	maj_stat = gss_display_name(&min_stat, server, &server_name,
				    &mechid);
	if (maj_stat != GSS_S_COMPLETE) {
	    /* "If the server rejects the security data (if
	       a checksum fails, for instance), it should
	       respond with reply code 535." */
	    reply_gss_error(R_535, maj_stat, min_stat,
			    "Extracting GSSAPI server name");
   	    gss_release_buffer(&accept_min, &out_tok);
            gss_release_cred(&acquire_min, &server_creds);
	    gss_release_name(&min_stat, &server);
            goto err;
	}
	/* If the server accepts the security data, but does
	   not require any additional data (i.e., the security
	   data exchange has completed successfully), it must
	   respond with reply code 235. */
	if (!replied) { 
	    pr_response_send(R_235, "GSSAPI Authentication succeeded");
	    gss_flags |= GSS_SESS_ADAT_OK;
        }
   	gss_release_buffer(&accept_min, &out_tok);
        gss_release_cred(&acquire_min, &server_creds);
	gss_release_name(&min_stat, &server);
      
	/* Install now our data channel NetIO handlers. */
	gss_netio_install_data();
        session.sp_flags = 0;

        gss_flags |= GSS_SESS_CONF_SUP;
        if ( !(ret_flags & GSS_C_REPLAY_FLAG || ret_flags & GSS_C_SEQUENCE_FLAG) ){
            gss_log("GSSAPI Warning: no replay protection !");
        }
        if ( !(ret_flags & GSS_C_SEQUENCE_FLAG) ){
            gss_log("GSSAPI Warning: no sequence protection !");
        }
        if ( !(ret_flags & GSS_C_CONF_FLAG) ){
            gss_log("GSSAPI Warning: no confidentiality service supported !");
            gss_flags &= ~GSS_SESS_CONF_SUP;
        }

        if (keytab_file) {
            gss_log("GSSAPI ReSet %s",keytab_file);
            putenv(pstrcat(main_server->pool, "KRB5_KTNAME=", keytab_file, NULL));
        } else {
            gss_log("GSSAPI UnSet KRB5_KTNAME");
            unsetenv("KRB5_KTNAME");
        } 
        return HANDLED(cmd);
    } else if (maj_stat == GSS_S_CONTINUE_NEEDED) {
	/* If the server accepts the security data, and
	   requires additional data, it should respond with
	   reply code 335. */
	pr_response_send(R_335, "more data needed");
   	gss_release_buffer(&accept_min, &out_tok);
        gss_release_cred(&acquire_min, &server_creds);
	gss_release_name(&min_stat, &server);
        if (keytab_file) {
            gss_log("GSSAPI ReSet %s",keytab_file);
            putenv(pstrcat(main_server->pool, "KRB5_KTNAME=", keytab_file, NULL));
        } else {
            gss_log("GSSAPI UnSet KRB5_KTNAME");
            unsetenv("KRB5_KTNAME");
        }
	return HANDLED(cmd);
    } else {
	/* "If the server rejects the security data (if
	   a checksum fails, for instance), it should
	   respond with reply code 535." */
	reply_gss_error(R_535, maj_stat, min_stat,
			"Failed processing ADAT");
   	gss_release_buffer(&accept_min, &out_tok);
        gss_release_cred(&acquire_min, &server_creds);
	gss_release_name(&min_stat, &server);
        goto err;
    }
    gss_release_buffer(&accept_min, &out_tok);
    gss_release_cred(&acquire_min, &server_creds);
    gss_release_name(&min_stat, &server);
    pr_response_add_err(R_535, "failed processing ADAT");
    gss_log("GSSAPI Failed processing ADAT");
err:
    if (keytab_file) {
        gss_log("GSSAPI ReSet %s",keytab_file);
        putenv(pstrcat(main_server->pool, "KRB5_KTNAME=", keytab_file, NULL));
    } else {
       gss_log("GSSAPI UnSet KRB5_KTNAME");
       unsetenv("KRB5_KTNAME");
    }
    return ERROR(cmd);
}

/*
   PROTECTION BUFFER SIZE (PBSZ)

      The argument is a decimal integer representing the maximum size,
      in bytes, of the encoded data blocks to be sent or received during
      file transfer.  This number shall be no greater than can be
      represented in a 32-bit unsigned integer.

      This command allows the FTP client and server to negotiate a
      maximum protected buffer size for the connection.  There is no
      default size; the client must issue a PBSZ command before it can
      issue the first PROT command.

      The PBSZ command must be preceded by a successful security data
      exchange.

      If the server cannot parse the argument, or if it will not fit in
      32 bits, it should respond with a 501 reply code.

      If the server has not completed a security data exchange with the
      client, it should respond with a 503 reply code.

      Otherwise, the server must reply with a 200 reply code.  If the
      size provided by the client is too large for the server, it must
      use a string of the form "PBSZ=number" in the text part of the
      reply to indicate a smaller buffer size.  The client and the
      server must use the smaller of the two buffer sizes if both buffer
      sizes are specified.

 */
MODRET gss_pbsz(cmd_rec *cmd) {

    if (!gss_engine)
	return DECLINED(cmd);

    CHECK_CMD_ARGS(cmd, 2);

    if ( session.rfc2228_mech && strcmp(session.rfc2228_mech, "GSSAPI") != 0 ) {
        return DECLINED(cmd);
    } else if (!(gss_flags & GSS_SESS_ADAT_OK)) {
	pr_response_add_err(R_503, "PBSZ not allowed on insecure control connection");
        gss_log("GSSAPI PBSZ not allowed on insecure control connection");
	return ERROR(cmd);
    }


    if (strlen(cmd->argv[1]) > 10 ||
	( strlen(cmd->argv[1]) == 10 && strcmp(cmd->argv[1],"4294967296") >= 0 )) {
	pr_response_add_err(R_501, "Bad value for PBSZ: %s", cmd->argv[1]);
	gss_log("GSSAPI Bad value for PBSZ: %s", cmd->argv[1]);
	return ERROR(cmd);
    } else if (actualbuf >= (maxbuf =(unsigned int) atol(cmd->argv[1]))) {
	pr_response_send(R_200, "PBSZ=%u", actualbuf);
	gss_log("GSSAPI Set PBSZ=%u", actualbuf);
    } else {
	actualbuf = (unsigned int) atol(cmd->argv[1]);
	/* I attempt what is asked for first, and if that
	   fails, I try dividing by 4 */
	while ((ucbuf = (unsigned char *)pcalloc(session.pool ? session.pool : permanent_pool, actualbuf)) == NULL) {
	    if (actualbuf) {
		pr_response_add(R_200, "Trying %u", actualbuf >>= 2);
		gss_log("GSSAPI Trying PBSZ=%u", actualbuf);
	    } else {
		pr_response_add_err(R_421, "Local resource failure: pcalloc");
		gss_log("GSSAPI Local resource failure: pcalloc");
		return ERROR(cmd);
	    }
	}
	pr_response_send(R_200, "PBSZ=%u", maxbuf = actualbuf);
	gss_log("GSSAPI Set PBSZ=%u", maxbuf);
    }

    gss_flags |= GSS_SESS_PBSZ_OK;
    return HANDLED(cmd);
}

/*
   DATA CHANNEL PROTECTION LEVEL (PROT)

      The argument is a single Telnet character code specifying the data
      channel protection level.

      This command indicates to the server what type of data channel
      protection the client and server will be using.  The following
      codes are assigned:

         C - Clear
         S - Safe
         E - Confidential
         P - Private

      The default protection level if no other level is specified is
      Clear.  The Clear protection level indicates that the data channel
      will carry the raw data of the file transfer, with no security
      applied.  The Safe protection level indicates that the data will
      be integrity protected.  The Confidential protection level
      indicates that the data will be confidentiality protected.  The
      Private protection level indicates that the data will be integrity
      and confidentiality protected.

      It is reasonable for a security mechanism not to provide all data
      channel protection levels.  It is also reasonable for a mechanism
      to provide more protection at a level than is required (for
      instance, a mechanism might provide Confidential protection, but
      include integrity-protection in that encoding, due to API or other
      considerations).

      The PROT command must be preceded by a successful protection
      buffer size negotiation.

      If the server does not understand the specified protection level,
      it should respond with reply code 504.

      If the current security mechanism does not support the specified
      protection level, the server should respond with reply code 536.

      If the server has not completed a protection buffer size
      negotiation with the client, it should respond with a 503 reply
      code.

      The PROT command will be rejected and the server should reply 503
      if no previous PBSZ command was issued.

      If the server is not willing to accept the specified protection
      level, it should respond with reply code 534.

      If the server is not able to accept the specified protection
      level, such as if a required resource is unavailable, it should
      respond with reply code 431.

      Otherwise, the server must reply with a 200 reply code to indicate
      that the specified protection level is accepted.

   CLEAR COMMAND CHANNEL (CCC)

      This command does not take an argument.

      It is desirable in some environments to use a security mechanism
      to authenticate and/or authorize the client and server, but not to
      perform any integrity checking on the subsequent commands.  This
      might be used in an environment where IP security is in place,
      insuring that the hosts are authenticated and that TCP streams
      cannot be tampered, but where user authentication is desired.

      If unprotected commands are allowed on any connection, then an
      attacker could insert a command on the control stream, and the
      server would have no way to know that it was invalid.  In order to
      prevent such attacks, once a security data exchange completes
      successfully, if the security mechanism supports integrity, then
      integrity (via the MIC or ENC command, and 631 or 632 reply) must
      be used, until the CCC command is issued to enable non-integrity
      protected control channel messages.  The CCC command itself must
      be integrity protected.

      Once the CCC command completes successfully, if a command is not
      protected, then the reply to that command must also not be
      protected.  This is to support interoperability with clients which
      do not support protection once the CCC command has been issued.

      This command must be preceded by a successful security data
      exchange.

      If the command is not integrity-protected, the server must respond
      with a 533 reply code.

      If the server is not willing to turn off the integrity
      requirement, it should respond with a 534 reply code.

      Otherwise, the server must reply with a 200 reply code to indicate
      that unprotected commands and replies may now be used on the
      command channel.

   INTEGRITY PROTECTED COMMAND (MIC) and
   CONFIDENTIALITY PROTECTED COMMAND (CONF) and
   PRIVACY PROTECTED COMMAND (ENC)

      The argument field of MIC is a Telnet string consisting of a base
      64 encoded "safe" message produced by a security mechanism
      specific message integrity procedure.  The argument field of CONF
      is a Telnet string consisting of a base 64 encoded "confidential"
      message produced by a security mechanism specific confidentiality
      procedure.  The argument field of ENC is a Telnet string
      consisting of a base 64 encoded "private" message produced by a
      security mechanism specific message integrity and confidentiality
      procedure.

      The server will decode and/or verify the encoded message.

      This command must be preceded by a successful security data
      exchange.

      A server may require that the first command after a successful
      security data exchange be CCC, and not implement the protection
      commands at all.  In this case, the server should respond with a
      502 reply code.

      If the server cannot base 64 decode the argument, it should
      respond with a 501 reply code.

      If the server has not completed a security data exchange with the
      client, it should respond with a 503 reply code.

      If the server has completed a security data exchange with the
      client using a mechanism which supports integrity, and requires a
      CCC command due to policy or implementation limitations, it should
      respond with a 503 reply code.

      If the server rejects the command because it is not supported by
      the current security mechanism, the server should respond with
      reply code 537.

      If the server rejects the command (if a checksum fails, for
      instance), it should respond with reply code 535.

      If the server is not willing to accept the command (if privacy is
      required by policy, for instance, or if a CONF command is received
      before a CCC command), it should respond with reply code 533.

      Otherwise, the command will be interpreted as an FTP command.  An
      end-of-line code need not be included, but if one is included, it
      must be a Telnet end-of-line code, not a local end-of-line code.

      The server may require that, under some or all circumstances, all
      commands be protected.  In this case, it should make a 533 reply
      to commands other than MIC, CONF, and ENC.

 */
MODRET gss_prot(cmd_rec *cmd) {
    int i;

    if (!gss_engine)
	return DECLINED(cmd);

    CHECK_CMD_ARGS(cmd, 2);

    if ( session.rfc2228_mech && strcmp(session.rfc2228_mech, "GSSAPI") != 0 ) {
        return DECLINED(cmd);
    } else if (!(gss_flags & GSS_SESS_PBSZ_OK)) {
   	pr_response_add_err(R_503, "You must issue the PBSZ command prior to PROT");
        gss_log("GSSAPI You must issue the PBSZ command prior to PROT");
	return ERROR(cmd);
    }

    /* Convert the parameter to upper case */
    for (i = 0; i < strlen(cmd->argv[1]); i++)
	(cmd->argv[1])[i] = toupper((cmd->argv[1])[i]);

    /* Only PROT S , PROT C or PROT P is valid with respect to GSS. */
    if (!strcmp(cmd->argv[1], "C")) {
	char *mesg = "Protection set to Clear";

	if (!gss_required_on_data) {

	    /* Only accept this if GSS is not required, by policy, on data
	     * connections.
	     */
   	    gss_prot_flags = GSS_SESS_PROT_C;
	    pr_response_add(R_200, "%s", mesg);
	    gss_log("GSSAPI %s", mesg);

	} else {
	    pr_response_add_err(R_534, "Unwilling to accept security parameters");
            gss_log("GSSAPI Unwilling to accept security parameters");
	    return ERROR(cmd);
	}

    } else if (!strcmp(cmd->argv[1], "P") && (gss_flags & GSS_SESS_CONF_SUP)) {
	char *mesg = "Protection set to Private";

	gss_prot_flags = GSS_SESS_PROT_P;
	pr_response_add(R_200, "%s", mesg);
	gss_log("GSSAPI %s", mesg);

    } else if (!strcmp(cmd->argv[1], "S")) {
	char *mesg = "Protection set to Safe";

	gss_prot_flags = GSS_SESS_PROT_S;
	pr_response_add(R_200, "%s", mesg);
	gss_log("GSSAPI %s", mesg);

    } else if (!strcmp(cmd->argv[1], "E"))  {
	char *mesg = "Protection NOT set to Confidential";
	gss_prot_flags = GSS_SESS_PROT_E;
	pr_response_add_err(R_536, "PROT E (Confidential) unsupported");
	gss_log("GSSAPI %s", mesg);
	return ERROR(cmd);

    } else {
	char *mesg = "Unsupported protection type";
	pr_response_add_err(R_504, "PROT %s unsupported", cmd->argv[1]);
	gss_log("GSSAPI %s %s", mesg,cmd->argv[1]);
	return ERROR(cmd);	    

    }

    return HANDLED(cmd);
}


/* Configuration handlers
 */

/* usage: GSSEngine on|off */
MODRET set_gssengine(cmd_rec *cmd) {
    int        bool = -1;
    config_rec *c = NULL;

    CHECK_ARGS(cmd, 1);
    CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

    if ((bool = get_boolean(cmd, 1)) == -1)
	CONF_ERROR(cmd, "expected Boolean parameter");

    c = add_config_param(cmd->argv[0], 1, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
    *((unsigned char *) c->argv[0]) = bool;

    return HANDLED(cmd);
}

/* usage: GSSKeytab file */
MODRET set_gsskeytab(cmd_rec *cmd) {

    CHECK_ARGS(cmd, 1);
    CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

    if (!file_exists(cmd->argv[1]))
	CONF_ERROR(cmd, "file does not exist");

    if (*cmd->argv[1] != '/')
	CONF_ERROR(cmd, "parameter must be an absolute path");

    add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
    return HANDLED(cmd);
}

/* usage: GSSLog file */
MODRET set_gsslog(cmd_rec *cmd) {

    CHECK_ARGS(cmd, 1);
    CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

    add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
    return HANDLED(cmd);
}

/* usage: GSSOptions opt1 opt2 ... */
MODRET set_gssoptions(cmd_rec *cmd) {
    config_rec *c = NULL;
    register unsigned int i = 0;
    unsigned long opts = 0UL;

    if (cmd->argc-1 == 0)
	CONF_ERROR(cmd, "wrong number of parameters");

    CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

    c = add_config_param(cmd->argv[0], 1, NULL);

    for (i = 1; i < cmd->argc; i++) {
       if (!strcasecmp(cmd->argv[i], "AllowCCC")) {
            opts |= GSS_OPT_ALLOW_CCC;
            pr_log_debug(DEBUG3, "GSSAPI GSSOption AllowCCC set");
       } else if (!strcasecmp(cmd->argv[i], "AllowFWCCC")) {
            opts |= GSS_OPT_ALLOW_FW_CCC;
    	    pr_feat_add("FWCCC");
            pr_log_debug(DEBUG3, "GSSAPI GSSOption AllowFWCCC set");
       } else if (!strcasecmp(cmd->argv[i], "AllowFWCCCOld")) {
            gss_flags |= GSS_SESS_FWCCC;
            pr_log_debug(DEBUG3, "GSSAPI GSSOption AllowFWCCCOld set");
       } else if (!strcasecmp(cmd->argv[i], "AllowFWNAT")) {
            opts |= GSS_OPT_ALLOW_FW_NAT;
            pr_log_debug(DEBUG3, "GSSAPI GSSOption AllowFWNAT set");
       } else if (!strcasecmp(cmd->argv[i], "NoChannelBinding")) {
            opts |= GSS_OPT_ALLOW_FW_NAT;
            pr_log_debug(DEBUG3, "GSSAPI GSSOption NoChannelBinding set");
       } else
            CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown GSSOption: '",
                                cmd->argv[i], "'", NULL));
    }

    c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
    *((unsigned long *) c->argv[0]) = opts;

    return HANDLED(cmd);
}

/* usage: GSSRequired on|off|both|ctrl|control|data */
MODRET set_gssrequired(cmd_rec *cmd) {
    int           bool = -1;
    unsigned char on_ctrl = FALSE, on_data = FALSE;
    config_rec    *c = NULL;

    CHECK_ARGS(cmd, 1);
    CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

    if ((bool = get_boolean(cmd, 1)) == -1) {

	if (!strcmp(cmd->argv[1], "control") || !strcmp(cmd->argv[1], "ctrl"))
	    on_ctrl = TRUE;

	else if (!strcmp(cmd->argv[1], "data"))
	    on_data = TRUE;

	else if (!strcmp(cmd->argv[1], "both")) {
	    on_ctrl = TRUE;
	    on_data = TRUE;
    
	} else
	    CONF_ERROR(cmd, "bad parameter");

    } else {
	if (bool == TRUE) {
	    on_ctrl = TRUE;
	    on_data = TRUE;
	}
    }

    if (on_ctrl) pr_log_debug(DEBUG3, "GSSAPI GSSRequired on ctrl channel set");
    if (on_data) pr_log_debug(DEBUG3, "GSSAPI GSSRequired on data channel set");
    c = add_config_param(cmd->argv[0], 2, NULL, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
    *((unsigned char *) c->argv[0]) = on_ctrl;
    c->argv[1] = pcalloc(c->pool, sizeof(unsigned char));
    *((unsigned char *) c->argv[1]) = on_data;

    return HANDLED(cmd);
}

/* Event handlers
 */

static void gss_postparse_ev(const void *event_data, void *user_data) {
    server_rec *s = NULL;
    int           res = 0;
    unsigned char *tmp = NULL;
    unsigned long *opts = NULL;
    config_rec    *c = NULL;

    for (s = (server_rec *) server_list->xas_list; s; s = s->next) {
       /* First, check to see whether mod_gss is even enabled. */
       if ((tmp = get_param_ptr(s->conf, "GSSEngine",
                                FALSE)) != NULL && *tmp == TRUE)
           gss_engine = TRUE;
       else {
           return;
       }

       /* Open the GSSLog, if configured */
       if ((res = gss_openlog(s)) < 0) {
           if (res == -1)
               pr_log_pri(LOG_NOTICE, MOD_GSS_VERSION ": notice: unable to open GSSLog: %s",
                       strerror(errno));

           else if (res == LOG_WRITEABLE_DIR)
               pr_log_pri(LOG_NOTICE, "notice: unable to open GSSLog: "
                       "parent directory is world writeable");

           else if (res == LOG_SYMLINK)
               pr_log_pri(LOG_NOTICE, "notice: unable to open GSSLog: "
                       "cannot log to a symbolic link");
       }

       /* Read GSSOptions */
       if ((opts = get_param_ptr(s->conf, "GSSOptions", FALSE)) != NULL)
           gss_opts = *opts;

       /* Read GSSRequired */
       if ((c = find_config(s->conf, CONF_PARAM, "GSSRequired",
                            FALSE))) {
           gss_required_on_ctrl = *((unsigned char *) c->argv[0]);
           gss_required_on_data = *((unsigned char *) c->argv[1]);
       }

       /* Get GSSkeytab file */
       gss_keytab_file = get_param_ptr(s->conf, "GSSKeytab", FALSE);
    }
    return;
}
static void gss_restart_ev(const void *event_data, void *user_data) {

  /* Re-register the postparse callback, to handle the (possibly changed)
   * configuration.
   */
  pr_event_register(&gss_module, "core.postparse", gss_postparse_ev, NULL);

  gss_closelog();
}

static void gss_sess_exit_ev(const void *event_data, void *user_data) {

    OM_uint32       maj_stat, min_stat;

    if (gss_data_netio) {
	destroy_pool(gss_data_netio->pool);
        gss_data_netio = NULL;
    }

    /* Unregister Netio functions for data channel */
    pr_unregister_netio(PR_NETIO_STRM_DATA);

    /* Delete security context */
    if ( gcontext ) {    
        gss_log("GSSAPI Delete security context %s",client_name.value ? (char *)client_name.value : "");
        maj_stat=gss_delete_sec_context(&min_stat,&gcontext,GSS_C_NO_BUFFER);
        if (maj_stat != GSS_S_COMPLETE) {
	    log_gss_error( maj_stat, min_stat,"could not delete credential");
            gss_release_buffer(&min_stat,GSS_C_NO_BUFFER);
        }
        gcontext = GSS_C_NO_CONTEXT;
    }

    gss_closelog();
    return;
}

/* Initialization routines
 */

static int gss_init(void) {

    /* Make sure the version of proftpd is as necessary. */
    if (PROFTPD_VERSION_NUMBER < 0x0001021001) {
	pr_log_pri(LOG_ERR, MOD_GSS_VERSION " requires proftpd 1.2.10rc1 and later");
	exit(1);
    }
    /* set command size buffer to maximum */
/* set in config file
   CmdBufSize=1023;
*/
    pr_feat_add("AUTH GSSAPI");
    pr_feat_add("ADAT");
    pr_feat_add("PBSZ");
    pr_feat_add("PROT");
    pr_feat_add("ENC");
    pr_feat_add("MIC");
    pr_feat_add("CONF");
    pr_feat_add("CCC");

    pr_event_register(&gss_module, "core.postparse", gss_postparse_ev, NULL);
    pr_event_register(&gss_module, "core.restart", gss_restart_ev, NULL);

    return 0;
}

static int gss_sess_init(void) {
    int           res = 0;
    unsigned char *tmp = NULL;
    unsigned long *opts = NULL;
    config_rec    *c = NULL;

    /* First, check to see whether mod_gss is even enabled. */
    if ((tmp = get_param_ptr(main_server->conf, "GSSEngine",
			     FALSE)) != NULL && *tmp == TRUE)
	gss_engine = TRUE;
    else {
	return 0;
    }

    /* Open the GSSLog, if configured */
    if ((res = gss_openlog(NULL)) < 0) {
	if (res == -1)
	    pr_log_pri(LOG_NOTICE, MOD_GSS_VERSION ": notice: unable to open GSSLog: %s",
		    strerror(errno));

	else if (res == LOG_WRITEABLE_DIR)
	    pr_log_pri(LOG_NOTICE, "notice: unable to open GSSLog: "
		    "parent directory is world writeable");

	else if (res == LOG_SYMLINK)
	    pr_log_pri(LOG_NOTICE, "notice: unable to open GSSLog: "
		    "cannot log to a symbolic link");
    }

    /* Read GSSOptions */
    if ((opts = get_param_ptr(main_server->conf, "GSSOptions", FALSE)) != NULL)
	gss_opts = *opts;

    /* Read GSSRequired */
    if ((c = find_config(main_server->conf, CONF_PARAM, "GSSRequired",
			 FALSE))) {
	gss_required_on_ctrl = *((unsigned char *) c->argv[0]);
	gss_required_on_data = *((unsigned char *) c->argv[1]);
    }

    /* Get GSSkeytab file */
    gss_keytab_file = get_param_ptr(main_server->conf, "GSSKeytab", FALSE);

    pr_event_register(&gss_module, "core.exit", gss_sess_exit_ev, NULL);

    return 0;
}

/* GSSAPI support functions
 */
/*
      One new reply type is introduced:

         6yz   Protected reply

            There are three reply codes of this type.  The first, reply
            code 631 indicates an integrity protected reply.  The
            second, reply code 632, indicates a confidentiality and
            integrity protected reply.  the third, reply code 633,
            indicates a confidentiality protected reply.

            The text part of a 631 reply is a Telnet string consisting
            of a base 64 encoded "safe" message produced by a security
            mechanism specific message integrity procedure.  The text
            part of a 632 reply is a Telnet string consisting of a base
            64 encoded "private" message produced by a security
            mechanism specific message confidentiality and integrity
            procedure.  The text part of a 633 reply is a Telnet string
            consisting of a base 64 encoded "confidential" message
            produced by a security mechanism specific message
            confidentiality procedure.

            The client will decode and verify the encoded reply.  How
            failures decoding or verifying replies are handled is
            implementation-specific.  An end-of-line code need not be
            included, but if one is included, it must be a Telnet end-
            of-line code, not a local end-of-line code.

            A protected reply may only be sent if a security data
            exchange has succeeded.

            The 63z reply may be a multiline reply.  In this case, the
            plaintext reply must be broken up into a number of
            fragments.  Each fragment must be protected, then base 64
            encoded in order into a separate line of the multiline
            reply.  There need not be any correspondence between the
            line breaks in the plaintext reply and the encoded reply.
            Telnet end-of-line codes must appear in the plaintext of the
            encoded reply, except for the final end-of-line code, which
            is optional.

            The multiline reply must be formatted more strictly than the
            continuation specification in RFC 959.  In particular, each
            line before the last must be formed by the reply code,
            followed immediately by a hyphen, followed by a base 64
            encoded fragment of the reply.

            For example, if the plaintext reply is

               123-First line
               Second line
                 234 A line beginning with numbers
               123 The last line

            then the resulting protected reply could be any of the
            following (the first example has a line break only to fit
            within the margins):

  631 base64(protect("123-First line\r\nSecond line\r\n  234 A line
  631-base64(protect("123-First line\r\n"))
  631-base64(protect("Second line\r\n"))
  631-base64(protect("  234 A line beginning with numbers\r\n"))
  631 base64(protect("123 The last line"))

  631-base64(protect("123-First line\r\nSecond line\r\n  234 A line b"))
  631 base64(protect("eginning with numbers\r\n123 The last line\r\n"))
*/
/* Appendix I

   The procedure associated with MIC commands, 631 replies, and Safe
   file transfers is:

      GSS_Wrap for the sender, with conf_flag == FALSE

      GSS_Unwrap for the receiver

   The procedure associated with ENC commands, 632 replies, and Private
   file transfers is:

      GSS_Wrap for the sender, with conf_flag == TRUE
      GSS_Unwrap for the receiver

   CONF commands and 633 replies are not supported.

   Both the client and server should inspect the value of conf_avail to
   determine whether the peer supports confidentiality services.

 */
static char *gss_format_cb(pool *pool, const char *fmt, ...)
{
    va_list msg;
    char buf[PR_TUNABLE_BUFFER_SIZE] = {'\0'};
    int  error = 0;
    int  conf_state;
    char *reply;
    
    gss_buffer_desc gss_in_buf, gss_out_buf;
    OM_uint32       maj_stat, min_stat;

    va_start(msg, fmt);
    vsnprintf(buf, sizeof(buf), fmt, msg);
    va_end(msg);
    buf[sizeof(buf)-1] = '\0';

    pr_log_debug(DEBUG9,"GSSAPI unwrapped response '%s'",buf);
    /* return buffer if no protection is set */
    if ( !session.sp_flags || (session.sp_flags & SP_CCC) )
          return pstrdup(pool ,buf);

    gss_in_buf.value = pstrdup(pool, buf);
    gss_in_buf.length = strlen(buf);

    /* protect response message */    
    maj_stat = gss_wrap(&min_stat, gcontext,
			session.sp_flags & SP_ENC, /* confidential */
			GSS_C_QOP_DEFAULT,
			&gss_in_buf, &conf_state,
			&gss_out_buf);

    if (maj_stat != GSS_S_COMPLETE) {
/* ??? how can this work?
 *      At this point of the conversation the client accepts 
 *      only protected responses. Since the server failed to 
 *      protect the response it will probably fail to protect
 *      the error response too.
        secure_gss_error(maj_stat, min_stat,
                        (session.sp_flags & SP_ENC)?
                        "gss_wrap ENC didn't complete":
                        (session.sp_flags & SP_MIC)?
                        "gss_wrap MIC didn't complete":
                        "gss_wrap didn't complete");
*/
	log_gss_error(maj_stat, min_stat, "could not seal/wrap reply");
	gss_release_buffer(&min_stat, &gss_out_buf);
        return buf;
    } else if ((session.sp_flags & SP_ENC) && !conf_state) {
/* ??? how can this work?
 *      At this point of the conversation the client accepts 
 *      only protected responses. Since the server failed to 
 *      protect the response it will probably fail to protect
 *      the error response too.
        secure_error("GSSAPI didn't protect message");
*/
	log_gss_error(maj_stat,min_stat,"could not protect message");
	gss_release_buffer(&min_stat, &gss_out_buf);
        return buf;
    } 
    /* protected reply <= 4*unprotected reply */
    reply=pcalloc(pool, gss_out_buf.length*4+1);
    if ((error = radix_encode(gss_out_buf.value, reply, (int *)&gss_out_buf.length, 0)) != 0 ) {
	gss_log("Couldn't encode reply (%s)", radix_error(error));
	gss_release_buffer(&min_stat, &gss_out_buf);
	return buf;
    } 
    gss_release_buffer(&min_stat, &gss_out_buf);
    
    /* add response code */
    reply=pstrcat(pool ,
                  session.sp_flags & SP_MIC ?  R_631 : 
                  session.sp_flags & SP_ENC ?  R_632 : 
  		  session.sp_flags & SP_CONF ?  R_633 : NULL,
                  " ",reply,"\r\n",NULL);
    pr_log_debug(DEBUG9,"GSSAPI wrapped response '%s'",reply);
    return reply;
}

/* dispatch unprotected command 
*/
static int gss_dispatch(char *buf)
{
    char *cp=buf, *wrd;
    cmd_rec *newcmd;
    pool *newpool;
    array_header *tarr;

    if (isspace((int)buf[0]))
	return 1;

    /* Nothing there...bail out.
     */
    if((wrd = pr_str_get_word(&cp,TRUE)) == NULL) 
       return 1;

    newpool = make_sub_pool(session.pool ? session.pool : permanent_pool);
    newcmd = (cmd_rec *) pcalloc(newpool,sizeof(cmd_rec));
    newcmd->pool = newpool;
    newcmd->stash_index = -1;

    tarr = make_array(newcmd->pool, 2, sizeof(char *));
    *((char **) push_array(tarr)) = pstrdup(newpool, wrd);
    newcmd->argc++;
    newcmd->arg = pstrdup(newpool, cp);

    while((wrd = pr_str_get_word(&cp,TRUE)) != NULL) {
      *((char **) push_array(tarr)) = pstrdup(newpool, wrd);
      newcmd->argc++;
    }

    *((char **) push_array(tarr)) = NULL;

    newcmd->argv = (char **) tarr->elts;

    pr_cmd_dispatch(newcmd);
    destroy_pool(newpool);

    return 0;   
}
/* check Kerberos 5  password
 */
static int kpass(char *name, char *passwd)
{
	krb5_error_code kerr;
        krb5_context kc;
	krb5_principal server, p;
	krb5_creds creds;
	krb5_timestamp now;

        kerr = krb5_init_context(&kc);
        if (kerr) {
            gss_log("GSSAPI Could not initialise krb5 context (%s)",error_message(kerr));
            return 1;
        }
        
        if (!name ) {
           gss_log("GSSAPI User name not set.");
           return 1;
        }

    	memset((char *)&creds, 0, sizeof(creds));
        kerr = krb5_parse_name(kc, name, &p);
        if (kerr) { 
            gss_log("GSSAPI Could not parse krb5 name (%s).",error_message(kerr));
            krb5_free_context(kc);
            return 1;
        }
	creds.client = p;

        if (!server_name.value ) {
           gss_log("GSSAPI Server name not set.");
           return 1;
        }

        kerr = krb5_parse_name(kc, server_name.value, &server);
        if (kerr) {
            gss_log("GSSAPI Could not parse krb5 server name (%s).",error_message(kerr));
            krb5_free_context(kc);
            return 1;
        }
	creds.server = server;

	kerr = krb5_timeofday(kc, &now);
        if (kerr) { 
            gss_log("GSSAPI Could not set krb5 time of day (%s).",error_message(kerr));
            krb5_free_context(kc);
            return 1;
        }

	creds.times.starttime = 0; /* start timer when 
					 request gets to KDC */
	creds.times.endtime = now + 60 * 60 * 10;
	creds.times.renew_till = 0;

#ifdef HAVE_INIT_CREDS_PASSWORD
        kerr = krb5_get_init_creds_password(kc, &creds, p, passwd,
                                            NULL, NULL,
                                            0, NULL, NULL);
        if (kerr) { 
            gss_log("GSSAPI Could not get krb5 ticket (%s).",error_message(kerr));
            krb5_free_context(kc);
            return 1;
        }	      
#endif

        krb5_free_context(kc);
	return 0;
}

/* write data to stream 
 */
static ssize_t looping_write(int fd, register const char *buf, int len)
{
    int cc;
    register int wrlen = len;
    do {
        cc = write(fd, buf, wrlen);
        if (cc < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return(cc);
        }
        else {
            buf += cc;
            wrlen -= cc;
        }
    } while (wrlen > 0);
    return(len);
}
/* read data from stream
 */
static ssize_t looping_read(int fd, register char *buf, int len)
{
    int cc, len2 = 0;

    do {
        cc = read(fd, buf, len);
	if (cc < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return(cc);          /* errno is already set */
        }
        else if (cc == 0) {
            return(len2);
        } else {
            buf += cc;
            len2 += cc;
            len -= cc;
        }
    } while (len > 0);
    return(len2);
}

/* base64 encode/decode
 */
/*
   Base 64 encoding is the same as the Printable Encoding described in
   Section 4.3.2.4 of [RFC-1421], except that line breaks must not be
   included. This encoding is defined as follows.

   Proceeding from left to right, the bit string resulting from the
   mechanism specific protection routine is encoded into characters
   which are universally representable at all sites, though not
   necessarily with the same bit patterns (e.g., although the character
   "E" is represented in an ASCII-based system as hexadecimal 45 and as
   hexadecimal C5 in an EBCDIC-based system, the local significance of
   the two representations is equivalent).

   A 64-character subset of International Alphabet IA5 is used, enabling
   6 bits to be represented per printable character.  (The proposed
   subset of characters is represented identically in IA5 and ASCII.)
   The character "=" signifies a special processing function used for
   padding within the printable encoding procedure.

   The encoding process represents 24-bit groups of input bits as output
   strings of 4 encoded characters.  Proceeding from left to right
   across a 24-bit input group output from the security mechanism
   specific message protection procedure, each 6-bit group is used as an
   index into an array of 64 printable characters, namely "[A-Z][a-
   z][0-9]+/".  The character referenced by the index is placed in the
   output string.  These characters are selected so as to be universally
   representable, and the set excludes characters with particular
   significance to Telnet (e.g., "<CR>", "<LF>", IAC).

   Special processing is performed if fewer than 24 bits are available
   in an input group at the end of a message.  A full encoding quantum
   is always completed at the end of a message.  When fewer than 24
   input bits are available in an input group, zero bits are added (on
   the right) to form an integral number of 6-bit groups.  Output
   character positions which are not required to represent actual input
   data are set to the character "=".  Since all canonically encoded
   output is an integral number of octets, only the following cases can
   arise: (1) the final quantum of encoding input is an integral
   multiple of 24 bits; here, the final unit of encoded output will be
   an integral multiple of 4 characters with no "=" padding, (2) the
   final quantum of encoding input is exactly 8 bits; here, the final
   unit of encoded output will be two characters followed by two "="
   padding characters, or (3) the final quantum of encoding input is
   exactly 16 bits; here, the final unit of encoded output will be three
   characters followed by one "=" padding character.

   Implementors must keep in mind that the base 64 encodings in ADAT,
   MIC, CONF, and ENC commands, and in 63z replies may be arbitrarily
   long.  Thus, the entire line must be read before it can be processed.
   Several successive reads on the control channel may be necessary.  It
   is not appropriate to for a server to reject a command containing a
   base 64 encoding simply because it is too long (assuming that the
   decoding is otherwise well formed in the context in which it was
   sent).

   Case must not be ignored when reading commands and replies containing
   base 64 encodings.
 */
static int radix_encode(unsigned char inbuf[], unsigned char outbuf[], int *len, int decode)
{
    int           i,j,D=0;
    char          *p;
    unsigned char c=0;

    if (decode) {
	for (i=0,j=0; inbuf[i] && inbuf[i] != '='; i++) {
	    if ((p = strchr(radixN, inbuf[i])) == NULL) return(1);
	    D = p - radixN;
	    switch (i&3) {
		case 0:
		    outbuf[j] = D<<2;
		    break;
		case 1:
		    outbuf[j++] |= D>>4;
		    outbuf[j] = (D&15)<<4;
		    break;
		case 2:
		    outbuf[j++] |= D>>2;
		    outbuf[j] = (D&3)<<6;
		    break;
		case 3:
		    outbuf[j++] |= D;
	    }
	}
	switch (i&3) {
	    case 1: return(3);
	    case 2: if (D&15) return(3);
		if (strcmp((char *)&inbuf[i], "==")) return(2);
		break;
	    case 3: if (D&3) return(3);
		if (strcmp((char *)&inbuf[i], "="))  return(2);
	}
	*len = j;
    } else {
	for (i=0,j=0; i < *len; i++)
	    switch (i%3) {
		case 0:
		    outbuf[j++] = radixN[inbuf[i]>>2];
		    c = (inbuf[i]&3)<<4;
		    break;
		case 1:
		    outbuf[j++] = radixN[c|inbuf[i]>>4];
		    c = (inbuf[i]&15)<<2;
		    break;
		case 2:
		    outbuf[j++] = radixN[c|inbuf[i]>>6];
		    outbuf[j++] = radixN[inbuf[i]&63];
		    c = 0;
	    }
	if (i%3) outbuf[j++] = radixN[c];
	switch (i%3) {
	    case 1: outbuf[j++] = '=';
	    case 2: outbuf[j++] = '=';
	}
	outbuf[*len = j] = '\0';
    }
    return(0);
}

/* base64 error messages
 */
static char *radix_error(e)
{
    switch (e) {
	case 0:  return("Success");
	case 1:  return("Bad character in encoding");
	case 2:  return("Encoding not properly padded");
	case 3:  return("Decoded # of bits not a multiple of 8");
	default: return("Unknown error");
    }
}

/* Send GSS error back to client
 */
static void reply_gss_error(char *code, OM_uint32 maj_stat, OM_uint32 min_stat, char *s)
{
    OM_uint32       gmaj_stat, gmin_stat;
    gss_buffer_desc msg;
    int             msg_ctx;

    /* log error first */
    log_gss_error(maj_stat, min_stat, s);

    msg_ctx = 0;
    while (!msg_ctx) {
	gmaj_stat = gss_display_status(&gmin_stat, maj_stat,
				       GSS_C_GSS_CODE,
				       GSS_C_NULL_OID,
				       &msg_ctx, &msg);
	if (gmaj_stat == GSS_S_COMPLETE) {
	    pr_response_add_err(code, "GSSAPI Error major: %s",
			     (char*)msg.value);
	    gss_release_buffer(&gmin_stat, &msg);
            break;
	}
        gss_release_buffer(&gmin_stat, &msg);
    } 
    msg_ctx = 0;
    while (!msg_ctx) {
	gmaj_stat = gss_display_status(&gmin_stat, min_stat,
				       GSS_C_MECH_CODE,
				       GSS_C_NULL_OID,
				       &msg_ctx, &msg);
	if (gmaj_stat == GSS_S_COMPLETE) {
	    pr_response_add_err(code, "GSSAPI Error minor: %s",
			     (char*)msg.value);
	    gss_release_buffer(&gmin_stat, &msg);
            break;
	}
        gss_release_buffer(&gmin_stat, &msg);
    }
    pr_response_add_err(code, "GSSAPI Error: %s", s);
}

/* Log GSS error to logfile
 */
static void log_gss_error(OM_uint32 maj_stat, OM_uint32 min_stat, char *s)
{

    /* a lot of work just to report the error */
    OM_uint32       gmaj_stat, gmin_stat;
    gss_buffer_desc msg;
    int             msg_ctx;

    msg_ctx = 0;
    while (!msg_ctx) {
	/* convert major status code (GSS-API error) to text */
	gmaj_stat = gss_display_status(&gmin_stat, maj_stat,
				       GSS_C_GSS_CODE,
				       GSS_C_NULL_OID,
				       &msg_ctx, &msg);
	if (gmaj_stat == GSS_S_COMPLETE) {
	    gss_log("GSSAPI Error major: %s",
		    (char*)msg.value);
	    gss_release_buffer(&gmin_stat, &msg);
            break;
	}
        gss_release_buffer(&gmin_stat, &msg);
    }
    msg_ctx = 0;
    while (!msg_ctx) {
	/* convert minor status code (underlying routine error) to text */
	gmaj_stat = gss_display_status(&gmin_stat, min_stat,
				       GSS_C_MECH_CODE,
				       GSS_C_NULL_OID,
				       &msg_ctx, &msg);
	if (gmaj_stat == GSS_S_COMPLETE) {
	    gss_log("GSSAPI Error minor: %s",
		    (char*)msg.value);
	    gss_release_buffer(&gmin_stat, &msg);
            break;
	}
        gss_release_buffer(&gmin_stat, &msg);
    }
    gss_log("GSSAPI Error: %s", s);
}

/* Module API tables
 */
static conftable gss_conftab[] = {
    { "GSSEngine",		set_gssengine,		NULL },
    { "GSSLog",			set_gsslog,		NULL },
    { "GSSKeytab",		set_gsskeytab,		NULL },
    { "GSSOptions",               set_gssoptions,         NULL },
    { "GSSRequired",              set_gssrequired,        NULL },
    { NULL , NULL, NULL}
};

static cmdtable gss_cmdtab[] = {
    { PRE_CMD,	C_ANY,	G_NONE,	gss_any,	FALSE,	FALSE },
    { CMD,	C_AUTH,	G_NONE,	gss_auth,	FALSE,	FALSE },
    { CMD,	C_ADAT,	G_NONE,	gss_adat,	FALSE,	FALSE },
    { CMD,	C_PBSZ,	G_NONE,	gss_pbsz,	FALSE,	FALSE },
    { CMD,	C_PROT,	G_NONE,	gss_prot,	FALSE,	FALSE },
    { CMD,      C_ENC,  G_NONE, gss_dec,        FALSE,   FALSE },
    { CMD,      C_MIC,  G_NONE, gss_dec,        FALSE,   FALSE },
    { CMD,      C_CONF, G_NONE, gss_dec,        FALSE,   FALSE },
    { CMD,	C_CCC,	G_NONE,	gss_ccc,	FALSE,	FALSE },
    { CMD,	C_FWCCC, G_NONE, gss_fwccc,	FALSE,	FALSE },
    { 0,	NULL }
};

static authtable gss_authtab[] = {
/* authenticate a user */
    { 0, "auth",	gss_authenticate },
/* check password */
    { 0, "check",	gss_auth_check },
/* unused authentication handlers */
/*
  { 0, "setpwent",    NULL  },
  { 0, "getpwent",    NULL  },
  { 0, "endpwent",    NULL  },
  { 0, "setgrent",    NULL  },
  { 0, "getgrent",    NULL  },
  { 0, "endgrent",    NULL  },
  { 0, "getpwnam",    NULL  },
  { 0, "getpwuid",    NULL  },
  { 0, "getgrnam",    NULL  },
  { 0, "getgrgid",    NULL  },
  { 0, "uid_name",    NULL  },
  { 0, "gid_name",    NULL  },
  { 0, "name_uid",    NULL  },
  { 0, "name_gid",    NULL  },
  { 0, "getgroups",   NULL  },
*/
    { 0, NULL }
};

module gss_module = {

    /* Always NULL */
    NULL, NULL,

    /* Module API version */
    0x20,

    /* Module name */
    "gss",

    /* Module configuration handler table */
    gss_conftab,

    /* Module command handler table */
    gss_cmdtab,

    /* Module authentication handler table */
    gss_authtab,

    /* Module initialization (global)*/
    gss_init,

    /* Session initialization (child)*/
    gss_sess_init
};

