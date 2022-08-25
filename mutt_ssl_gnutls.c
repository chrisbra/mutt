/* Copyright (C) 2001 Marco d'Itri <md@linux.it>
 * Copyright (C) 2001-2004 Andrew McDonald <andrew@mcdonald.org.uk>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#ifdef HAVE_GNUTLS_OPENSSL_H
#include <gnutls/openssl.h>
#endif

#include "mutt.h"
#include "mutt_socket.h"
#include "mutt_curses.h"
#include "mutt_menu.h"
#include "mutt_ssl.h"
#include "mutt_regex.h"

/* certificate error bitmap values */
#define CERTERR_VALID       0
#define CERTERR_EXPIRED     1
#define CERTERR_NOTYETVALID (1<<1)
#define CERTERR_REVOKED     (1<<2)
#define CERTERR_NOTTRUSTED  (1<<3)
#define CERTERR_HOSTNAME    (1<<4)
#define CERTERR_SIGNERNOTCA (1<<5)
#define CERTERR_INSECUREALG (1<<6)
#define CERTERR_OTHER       (1<<7)

/* deprecated types compatibility */

#ifndef HAVE_GNUTLS_CERTIFICATE_CREDENTIALS_T
typedef gnutls_certificate_credentials gnutls_certificate_credentials_t;
#endif

#ifndef HAVE_GNUTLS_CERTIFICATE_STATUS_T
typedef gnutls_certificate_status gnutls_certificate_status_t;
#endif

#ifndef HAVE_GNUTLS_DATUM_T
typedef gnutls_datum gnutls_datum_t;
#endif

#ifndef HAVE_GNUTLS_DIGEST_ALGORITHM_T
typedef gnutls_digest_algorithm gnutls_digest_algorithm_t;
#endif

#ifndef HAVE_GNUTLS_SESSION_T
typedef gnutls_session gnutls_session_t;
#endif

#ifndef HAVE_GNUTLS_TRANSPORT_PTR_T
typedef gnutls_transport_ptr gnutls_transport_ptr_t;
#endif

#ifndef HAVE_GNUTLS_X509_CRT_T
typedef gnutls_x509_crt gnutls_x509_crt_t;
#endif


typedef struct _tlssockdata
{
  gnutls_session_t state;
  gnutls_certificate_credentials_t xcred;
}
tlssockdata;

/* local prototypes */
static int tls_socket_read (CONNECTION* conn, char* buf, size_t len);
static int tls_socket_write (CONNECTION* conn, const char* buf, size_t len);
static int tls_socket_poll (CONNECTION* conn, time_t wait_secs);
static int tls_socket_open (CONNECTION* conn);
static int tls_socket_close (CONNECTION* conn);
static int tls_starttls_close (CONNECTION* conn);

static int tls_init (void);
static int tls_negotiate (CONNECTION* conn);
static int tls_check_certificate (CONNECTION* conn);
static int tls_passwd_cb (void* userdata, int attempt, const char* token_url,
                          const char* token_label,
                          unsigned int flags,
                          char* pin, size_t pin_max);


static int tls_init (void)
{
  static unsigned char init_complete = 0;
  int err;

  if (init_complete)
    return 0;

  err = gnutls_global_init ();
  if (err < 0)
  {
    mutt_error ("gnutls_global_init: %s", gnutls_strerror (err));
    mutt_sleep (2);
    return -1;
  }

  init_complete = 1;
  return 0;
}

int mutt_ssl_socket_setup (CONNECTION* conn)
{
  if (tls_init () < 0)
    return -1;

  conn->conn_open	= tls_socket_open;
  conn->conn_read	= tls_socket_read;
  conn->conn_write	= tls_socket_write;
  conn->conn_close	= tls_socket_close;
  conn->conn_poll       = tls_socket_poll;

  return 0;
}

static int tls_socket_read (CONNECTION* conn, char* buf, size_t len)
{
  tlssockdata *data = conn->sockdata;
  int ret;

  if (!data)
  {
    mutt_error (_("Error: no TLS socket open"));
    mutt_sleep (2);
    return -1;
  }

  do
  {
    ret = gnutls_record_recv (data->state, buf, len);
  } while (ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED);

  if (ret < 0)
  {
    mutt_error ("tls_socket_read (%s)", gnutls_strerror (ret));
    mutt_sleep (4);
    return -1;
  }

  return ret;
}

static int tls_socket_write (CONNECTION* conn, const char* buf, size_t len)
{
  tlssockdata *data = conn->sockdata;
  int ret;
  size_t sent = 0;

  if (!data)
  {
    mutt_error (_("Error: no TLS socket open"));
    mutt_sleep (2);
    return -1;
  }

  do
  {
    do
    {
      ret = gnutls_record_send (data->state, buf + sent, len - sent);
    } while (ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED);

    if (ret < 0)
    {
      mutt_error ("tls_socket_write (%s)", gnutls_strerror (ret));
      mutt_sleep (4);
      return -1;
    }

    sent += ret;
  } while (sent < len);

  return sent;
}

static int tls_socket_poll (CONNECTION* conn, time_t wait_secs)
{
  tlssockdata *data = conn->sockdata;

  if (!data)
    return -1;

  if (gnutls_record_check_pending (data->state))
    return 1;
  else
    return raw_socket_poll (conn, wait_secs);
}

static int tls_socket_open (CONNECTION* conn)
{
  if (raw_socket_open (conn) < 0)
    return -1;

  if (tls_negotiate (conn) < 0)
  {
    tls_socket_close (conn);
    return -1;
  }

  return 0;
}

int mutt_ssl_starttls (CONNECTION* conn)
{
  if (mutt_socket_has_buffered_input (conn))
  {
    /* L10N:
       The server is not supposed to send data immediately after
       confirming STARTTLS.  This warns the user that something
       weird is going on.
    */
    mutt_error _("Warning: clearing unexpected server data before TLS negotiation");
    mutt_sleep (0);
    mutt_socket_clear_buffered_input (conn);
  }

  if (tls_init () < 0)
    return -1;

  if (tls_negotiate (conn) < 0)
    return -1;

  conn->conn_read	= tls_socket_read;
  conn->conn_write	= tls_socket_write;
  conn->conn_close	= tls_starttls_close;
  conn->conn_poll       = tls_socket_poll;

  return 0;
}

/* Note: this function grabs the CN out of the client
 * cert but appears to do nothing with it.
 *
 * It does contain a call to mutt_account_getuser(), but this
 * interferes with SMTP client-cert authentication that doesn't use
 * AUTH EXTERNAL. (see gitlab #336)
 *
 * The mutt_sasl.c code sets up callbacks to get the login or user,
 * and it looks like the Cyrus SASL external code calls those.
 *
 * Brendan doesn't recall if this really was necessary at one time, so
 * I'm disabling it.
 */
#if 0
static void tls_get_client_cert (CONNECTION* conn)
{
  tlssockdata *data = conn->sockdata;
  const gnutls_datum_t* crtdata;
  gnutls_x509_crt_t clientcrt;
  char* cn = NULL;
  size_t cnlen = 0;
  int rc;

  /* get our cert CN if we have one */
  if (!(crtdata = gnutls_certificate_get_ours (data->state)))
    return;

  if (gnutls_x509_crt_init (&clientcrt) < 0)
  {
    dprint (1, (debugfile, "Failed to init gnutls crt\n"));
    return;
  }
  if (gnutls_x509_crt_import (clientcrt, crtdata, GNUTLS_X509_FMT_DER) < 0)
  {
    dprint (1, (debugfile, "Failed to import gnutls client crt\n"));
    goto err;
  }

  /* get length of CN, then grab it. */
  rc = gnutls_x509_crt_get_dn_by_oid (clientcrt, GNUTLS_OID_X520_COMMON_NAME,
                                      0, 0, NULL, &cnlen);
  if (((rc >= 0) || (rc == GNUTLS_E_SHORT_MEMORY_BUFFER)) &&
      cnlen > 0)
  {
    cn = safe_calloc (1, cnlen);
    if (gnutls_x509_crt_get_dn_by_oid (clientcrt, GNUTLS_OID_X520_COMMON_NAME,
                                       0, 0, cn, &cnlen) < 0)
      goto err;
    dprint (2, (debugfile, "client certificate CN: %s\n", cn));

    /* if we are using a client cert, SASL may expect an external auth name */
    mutt_account_getuser (&conn->account);
  }

err:
  FREE (&cn);
  gnutls_x509_crt_deinit (clientcrt);
}
#endif

#if HAVE_GNUTLS_PRIORITY_SET_DIRECT
static int tls_set_priority (tlssockdata *data)
{
  size_t nproto = 5;
  BUFFER *priority = NULL;
  int err, rv = -1;

  priority = mutt_buffer_pool_get ();

  if (SslCiphers)
    mutt_buffer_strcpy (priority, SslCiphers);
  else
    mutt_buffer_strcpy (priority, "NORMAL");

  if (!option (OPTTLSV1_3))
  {
    nproto--;
    mutt_buffer_addstr (priority, ":-VERS-TLS1.3");
  }
  if (!option (OPTTLSV1_2))
  {
    nproto--;
    mutt_buffer_addstr (priority, ":-VERS-TLS1.2");
  }
  if (!option (OPTTLSV1_1))
  {
    nproto--;
    mutt_buffer_addstr (priority, ":-VERS-TLS1.1");
  }
  if (!option (OPTTLSV1))
  {
    nproto--;
    mutt_buffer_addstr (priority, ":-VERS-TLS1.0");
  }
  if (!option (OPTSSLV3))
  {
    nproto--;
    mutt_buffer_addstr (priority, ":-VERS-SSL3.0");
  }

  if (nproto == 0)
  {
    mutt_error (_("All available protocols for TLS/SSL connection disabled"));
    goto cleanup;
  }

  if ((err = gnutls_priority_set_direct (data->state, mutt_b2s (priority), NULL)) < 0)
  {
    mutt_error ("gnutls_priority_set_direct(%s): %s", mutt_b2s (priority), gnutls_strerror (err));
    mutt_sleep (2);
    goto cleanup;
  }

  rv = 0;

cleanup:
  mutt_buffer_pool_release (&priority);
  return rv;
}
#else
/* This array needs to be large enough to hold all the possible values support
 * by Mutt.  The initialized values are just placeholders--the array gets
 * overwrriten in tls_negotiate() depending on the $ssl_use_* options.
 *
 * Note: gnutls_protocol_set_priority() was removed in GnuTLS version
 * 3.4 (2015-04).  TLS 1.3 support wasn't added until version 3.6.5.
 * Therefore, no attempt is made to support $ssl_use_tlsv1_3 in this code.
 */
static int protocol_priority[] = {GNUTLS_TLS1_2, GNUTLS_TLS1_1, GNUTLS_TLS1, GNUTLS_SSL3, 0};

static int tls_set_priority (tlssockdata *data)
{
  size_t nproto = 0; /* number of tls/ssl protocols */

  if (option (OPTTLSV1_2))
    protocol_priority[nproto++] = GNUTLS_TLS1_2;
  if (option (OPTTLSV1_1))
    protocol_priority[nproto++] = GNUTLS_TLS1_1;
  if (option (OPTTLSV1))
    protocol_priority[nproto++] = GNUTLS_TLS1;
  if (option (OPTSSLV3))
    protocol_priority[nproto++] = GNUTLS_SSL3;
  protocol_priority[nproto] = 0;

  if (nproto == 0)
  {
    mutt_error (_("All available protocols for TLS/SSL connection disabled"));
    return -1;
  }

  if (SslCiphers)
  {
    mutt_error (_("Explicit ciphersuite selection via $ssl_ciphers not supported"));
    mutt_sleep (2);
  }

  /* We use default priorities (see gnutls documentation),
     except for protocol version */
  gnutls_set_default_priority (data->state);
  gnutls_protocol_set_priority (data->state, protocol_priority);
  return 0;
}
#endif

/* tls_negotiate: After TLS state has been initialized, attempt to negotiate
 *   TLS over the wire, including certificate checks. */
static int tls_negotiate (CONNECTION * conn)
{
  tlssockdata *data;
  int err;
  char *hostname;

  data = (tlssockdata *) safe_calloc (1, sizeof (tlssockdata));
  conn->sockdata = data;
  err = gnutls_certificate_allocate_credentials (&data->xcred);
  if (err < 0)
  {
    FREE (&conn->sockdata);
    mutt_error ("gnutls_certificate_allocate_credentials: %s", gnutls_strerror (err));
    mutt_sleep (2);
    return -1;
  }
  gnutls_certificate_set_pin_function (data->xcred, tls_passwd_cb,
                                       &conn->account);

  gnutls_certificate_set_x509_trust_file (data->xcred, SslCertFile,
					  GNUTLS_X509_FMT_PEM);
  /* ignore errors, maybe file doesn't exist yet */

  if (SslCACertFile)
  {
    gnutls_certificate_set_x509_trust_file (data->xcred, SslCACertFile,
                                            GNUTLS_X509_FMT_PEM);
  }

  if (SslClientCert)
  {
    dprint (2, (debugfile, "Using client certificate %s\n", SslClientCert));
    gnutls_certificate_set_x509_key_file (data->xcred, SslClientCert,
                                          SslClientCert, GNUTLS_X509_FMT_PEM);
  }

#if HAVE_DECL_GNUTLS_VERIFY_DISABLE_TIME_CHECKS
  /* disable checking certificate activation/expiration times
     in gnutls, we do the checks ourselves */
  gnutls_certificate_set_verify_flags (data->xcred, GNUTLS_VERIFY_DISABLE_TIME_CHECKS);
#endif

  if ((err = gnutls_init (&data->state, GNUTLS_CLIENT)))
  {
    mutt_error ("gnutls_init(): %s", gnutls_strerror (err));
    mutt_sleep (2);
    goto fail;
  }

  /* set socket */
  gnutls_transport_set_ptr (data->state, (gnutls_transport_ptr_t)(long)conn->fd);

  hostname = SslVerifyHostOverride ? SslVerifyHostOverride : conn->account.host;
  if (gnutls_server_name_set (data->state, GNUTLS_NAME_DNS, hostname,
                              mutt_strlen (hostname)))
  {
    mutt_error _("Warning: unable to set TLS SNI host name");
    mutt_sleep (1);
  }

  if (tls_set_priority (data) < 0)
  {
    goto fail;
  }

  if (SslDHPrimeBits > 0)
  {
    gnutls_dh_set_prime_bits (data->state, SslDHPrimeBits);
  }

  gnutls_credentials_set (data->state, GNUTLS_CRD_CERTIFICATE, data->xcred);

  do
  {
    err = gnutls_handshake (data->state);
  } while (err == GNUTLS_E_AGAIN || err == GNUTLS_E_INTERRUPTED);

  if (err < 0)
  {
    if (err == GNUTLS_E_FATAL_ALERT_RECEIVED)
    {
      mutt_error ("gnutls_handshake: %s(%s)", gnutls_strerror (err),
                  gnutls_alert_get_name (gnutls_alert_get (data->state)));
    }
    else
    {
      mutt_error ("gnutls_handshake: %s", gnutls_strerror (err));
    }
    mutt_sleep (2);
    goto fail;
  }

  if (!tls_check_certificate (conn))
    goto fail;

  /* set Security Strength Factor (SSF) for SASL */
  /* NB: gnutls_cipher_get_key_size() returns key length in bytes */
  conn->ssf = gnutls_cipher_get_key_size (gnutls_cipher_get (data->state)) * 8;

#if 0
  /* See comment above the tls_get_client_cert() function for why this
   * is ifdef'ed out.  Also note the SslClientCert is already set up
   * above. */
  tls_get_client_cert (conn);
#endif

  if (!option (OPTNOCURSES))
  {
    mutt_message (_("SSL/TLS connection using %s (%s/%s/%s)"),
                  gnutls_protocol_get_name (gnutls_protocol_get_version (data->state)),
                  gnutls_kx_get_name (gnutls_kx_get (data->state)),
                  gnutls_cipher_get_name (gnutls_cipher_get (data->state)),
                  gnutls_mac_get_name (gnutls_mac_get (data->state)));
    mutt_sleep (0);
  }

  return 0;

fail:
  gnutls_certificate_free_credentials (data->xcred);
  gnutls_deinit (data->state);
  FREE (&conn->sockdata);
  return -1;
}

static int tls_socket_close (CONNECTION* conn)
{
  tlssockdata *data = conn->sockdata;
  if (data)
  {
    /* shut down only the write half to avoid hanging waiting for the remote to respond.
     *
     * RFC5246 7.2.1. "Closure Alerts"
     *
     * It is not required for the initiator of the close to wait for the
     * responding close_notify alert before closing the read side of the
     * connection.
     */
    gnutls_bye (data->state, GNUTLS_SHUT_WR);

    gnutls_certificate_free_credentials (data->xcred);
    gnutls_deinit (data->state);
    FREE (&conn->sockdata);
  }

  return raw_socket_close (conn);
}

static int tls_starttls_close (CONNECTION* conn)
{
  int rc;

  rc = tls_socket_close (conn);
  conn->conn_read = raw_socket_read;
  conn->conn_write = raw_socket_write;
  conn->conn_close = raw_socket_close;
  conn->conn_poll = raw_socket_poll;

  return rc;
}

#define CERT_SEP "-----BEGIN"

/* this bit is based on read_ca_file() in gnutls */
static int tls_compare_certificates (const gnutls_datum_t *peercert)
{
  gnutls_datum_t cert;
  unsigned char *ptr;
  FILE *fd1;
  int ret;
  gnutls_datum_t b64_data;
  unsigned char *b64_data_data;
  struct stat filestat;

  if (stat (SslCertFile, &filestat) == -1)
    return 0;

  b64_data.size = filestat.st_size+1;
  b64_data_data = (unsigned char *) safe_calloc (1, b64_data.size);
  b64_data_data[b64_data.size-1] = '\0';
  b64_data.data = b64_data_data;

  fd1 = fopen (SslCertFile, "r");
  if (fd1 == NULL)
  {
    return 0;
  }

  b64_data.size = fread (b64_data.data, 1, b64_data.size, fd1);
  safe_fclose (&fd1);

  do
  {
    ret = gnutls_pem_base64_decode_alloc (NULL, &b64_data, &cert);
    if (ret != 0)
    {
      FREE (&b64_data_data);
      return 0;
    }

    /* find start of cert, skipping junk */
    ptr = (unsigned char *)strstr ((char*)b64_data.data, CERT_SEP);
    if (!ptr)
    {
      gnutls_free (cert.data);
      FREE (&b64_data_data);
      return 0;
    }
    /* find start of next cert */
    ptr = (unsigned char *)strstr ((char*)ptr + 1, CERT_SEP);

    b64_data.size = b64_data.size - (ptr - b64_data.data);
    b64_data.data = ptr;

    if (cert.size == peercert->size)
    {
      if (memcmp (cert.data, peercert->data, cert.size) == 0)
      {
	/* match found */
        gnutls_free (cert.data);
	FREE (&b64_data_data);
	return 1;
      }
    }

    gnutls_free (cert.data);
  } while (ptr != NULL);

  /* no match found */
  FREE (&b64_data_data);
  return 0;
}

static void tls_fingerprint (gnutls_digest_algorithm_t algo,
                             char* s, int l, const gnutls_datum_t* data)
{
  unsigned char md[64];
  size_t n;
  int j;

  n = 64;

  if (gnutls_fingerprint (algo, data, (char *)md, &n) < 0)
  {
    snprintf (s, l, _("[unable to calculate]"));
  }
  else
  {
    for (j = 0; j < (int) n; j++)
    {
      char ch[8];
      snprintf (ch, 8, "%02X%s", md[j], (j % 2 ? " " : ""));
      safe_strcat (s, l, ch);
    }
    s[2*n+n/2-1] = '\0'; /* don't want trailing space */
  }
}

static char *tls_make_date (time_t t, char *s, size_t len)
{
  struct tm *l = gmtime (&t);

  if (l)
    snprintf (s, len,  "%s, %d %s %d %02d:%02d:%02d UTC",
	      Weekdays[l->tm_wday], l->tm_mday, Months[l->tm_mon],
	      l->tm_year + 1900, l->tm_hour, l->tm_min, l->tm_sec);
  else
    strfcpy (s, _("[invalid date]"), len);

  return (s);
}

static int tls_check_stored_hostname (const gnutls_datum_t *cert,
                                      const char *hostname)
{
  char buf[80];
  FILE *fp;
  char *linestr = NULL;
  size_t linestrsize;
  int linenum = 0;
  regex_t preg;
  regmatch_t pmatch[3];

  /* try checking against names stored in stored certs file */
  if ((fp = fopen (SslCertFile, "r")))
  {
    if (REGCOMP (&preg, "^#H ([a-zA-Z0-9_\\.-]+) ([0-9A-F]{4}( [0-9A-F]{4}){7})[ \t]*$",
                 REG_ICASE) != 0)
    {
      safe_fclose (&fp);
      return 0;
    }

    buf[0] = '\0';
    tls_fingerprint (GNUTLS_DIG_MD5, buf, sizeof (buf), cert);
    while ((linestr = mutt_read_line (linestr, &linestrsize, fp, &linenum, 0)) != NULL)
    {
      if (linestr[0] == '#' && linestr[1] == 'H')
      {
        if (regexec (&preg, linestr, 3, pmatch, 0) == 0)
        {
          linestr[pmatch[1].rm_eo] = '\0';
          linestr[pmatch[2].rm_eo] = '\0';
          if (strcmp (linestr + pmatch[1].rm_so, hostname) == 0 &&
              strcmp (linestr + pmatch[2].rm_so, buf) == 0)
          {
            regfree (&preg);
            FREE (&linestr);
            safe_fclose (&fp);
            return 1;
          }
        }
      }
    }

    regfree (&preg);
    safe_fclose (&fp);
  }

  /* not found a matching name */
  return 0;
}

/* Returns 0 on success
 *        -1 on failure
 */
static int tls_check_preauth (const gnutls_datum_t *certdata,
                              gnutls_certificate_status_t certstat,
                              const char *hostname, int chainidx, int* certerr,
                              int* savedcert)
{
  gnutls_x509_crt_t cert;

  *certerr = CERTERR_VALID;
  *savedcert = 0;

  if (gnutls_x509_crt_init (&cert) < 0)
  {
    mutt_error (_("Error initialising gnutls certificate data"));
    mutt_sleep (2);
    return -1;
  }

  if (gnutls_x509_crt_import (cert, certdata, GNUTLS_X509_FMT_DER) < 0)
  {
    mutt_error (_("Error processing certificate data"));
    mutt_sleep (2);
    gnutls_x509_crt_deinit (cert);
    return -1;
  }

  /* Note: tls_negotiate() contains a call to
   * gnutls_certificate_set_verify_flags() with a flag disabling
   * GnuTLS checking of the dates.  So certstat shouldn't have the
   * GNUTLS_CERT_EXPIRED and GNUTLS_CERT_NOT_ACTIVATED bits set. */
  if (option (OPTSSLVERIFYDATES) != MUTT_NO)
  {
    if (gnutls_x509_crt_get_expiration_time (cert) < time (NULL))
      *certerr |= CERTERR_EXPIRED;
    if (gnutls_x509_crt_get_activation_time (cert) > time (NULL))
      *certerr |= CERTERR_NOTYETVALID;
  }

  if (chainidx == 0 && option (OPTSSLVERIFYHOST) != MUTT_NO
      && !gnutls_x509_crt_check_hostname (cert, hostname)
      && !tls_check_stored_hostname (certdata, hostname))
    *certerr |= CERTERR_HOSTNAME;

  if (certstat & GNUTLS_CERT_REVOKED)
  {
    *certerr |= CERTERR_REVOKED;
    certstat ^= GNUTLS_CERT_REVOKED;
  }

  /* see whether certificate is in our cache (certificates file) */
  if (tls_compare_certificates (certdata))
  {
    *savedcert = 1;

    /* We check above for certs with bad dates or that are revoked.
     * These must be accepted manually each time.  Otherwise, we
     * accept saved certificates as valid. */
    if (*certerr == CERTERR_VALID)
    {
      gnutls_x509_crt_deinit (cert);
      return 0;
    }
  }

  if (certstat & GNUTLS_CERT_INVALID)
  {
    *certerr |= CERTERR_NOTTRUSTED;
    certstat ^= GNUTLS_CERT_INVALID;
  }

  if (certstat & GNUTLS_CERT_SIGNER_NOT_FOUND)
  {
    /* NB: already cleared if cert in cache */
    *certerr |= CERTERR_NOTTRUSTED;
    certstat ^= GNUTLS_CERT_SIGNER_NOT_FOUND;
  }

  if (certstat & GNUTLS_CERT_SIGNER_NOT_CA)
  {
    /* NB: already cleared if cert in cache */
    *certerr |= CERTERR_SIGNERNOTCA;
    certstat ^= GNUTLS_CERT_SIGNER_NOT_CA;
  }

  if (certstat & GNUTLS_CERT_INSECURE_ALGORITHM)
  {
    /* NB: already cleared if cert in cache */
    *certerr |= CERTERR_INSECUREALG;
    certstat ^= GNUTLS_CERT_INSECURE_ALGORITHM;
  }

  /* we've been zeroing the interesting bits in certstat -
   * don't return OK if there are any unhandled bits we don't
   * understand */
  if (certstat != 0)
    *certerr |= CERTERR_OTHER;

  gnutls_x509_crt_deinit (cert);

  if (*certerr == CERTERR_VALID)
    return 0;

  return -1;
}

/* Returns 1 on success.
 *         0 on failure.
 */
static int tls_check_one_certificate (const gnutls_datum_t *certdata,
                                      gnutls_certificate_status_t certstat,
                                      const char* hostname, int idx, int len)
{
  int certerr, savedcert;
  gnutls_x509_crt_t cert;
  char buf[SHORT_STRING];
  char fpbuf[SHORT_STRING];
  size_t buflen;
  char dn_common_name[SHORT_STRING];
  char dn_email[SHORT_STRING];
  char dn_organization[SHORT_STRING];
  char dn_organizational_unit[SHORT_STRING];
  char dn_locality[SHORT_STRING];
  char dn_province[SHORT_STRING];
  char dn_country[SHORT_STRING];
  time_t t;
  char datestr[30];
  MUTTMENU *menu;
  char helpstr[LONG_STRING];
  char title[STRING];
  BUFFER *drow = NULL;
  FILE *fp;
  gnutls_datum_t pemdata;
  int done, ret, reset_ignoremacro = 0;

  if (!tls_check_preauth (certdata, certstat, hostname, idx, &certerr,
                          &savedcert))
    return 1;

  if (option (OPTNOCURSES))
  {
    dprint (1, (debugfile, "tls_check_one_certificate: unable to prompt for certificate in batch mode\n"));
    mutt_error _("Untrusted server certificate");
    return 0;
  }

  /* interactive check from user */
  if (gnutls_x509_crt_init (&cert) < 0)
  {
    mutt_error (_("Error initialising gnutls certificate data"));
    mutt_sleep (2);
    return 0;
  }

  if (gnutls_x509_crt_import (cert, certdata, GNUTLS_X509_FMT_DER) < 0)
  {
    mutt_error (_("Error processing certificate data"));
    mutt_sleep (2);
    gnutls_x509_crt_deinit (cert);
    return 0;
  }

  drow = mutt_buffer_pool_get ();

  menu = mutt_new_menu (MENU_GENERIC);
  mutt_push_current_menu (menu);


  buflen = sizeof (dn_common_name);
  if (gnutls_x509_crt_get_dn_by_oid (cert, GNUTLS_OID_X520_COMMON_NAME, 0, 0,
                                     dn_common_name, &buflen) != 0)
    dn_common_name[0] = '\0';
  buflen = sizeof (dn_email);
  if (gnutls_x509_crt_get_dn_by_oid (cert, GNUTLS_OID_PKCS9_EMAIL, 0, 0,
                                     dn_email, &buflen) != 0)
    dn_email[0] = '\0';
  buflen = sizeof (dn_organization);
  if (gnutls_x509_crt_get_dn_by_oid (cert, GNUTLS_OID_X520_ORGANIZATION_NAME, 0, 0,
                                     dn_organization, &buflen) != 0)
    dn_organization[0] = '\0';
  buflen = sizeof (dn_organizational_unit);
  if (gnutls_x509_crt_get_dn_by_oid (cert, GNUTLS_OID_X520_ORGANIZATIONAL_UNIT_NAME, 0, 0,
                                     dn_organizational_unit, &buflen) != 0)
    dn_organizational_unit[0] = '\0';
  buflen = sizeof (dn_locality);
  if (gnutls_x509_crt_get_dn_by_oid (cert, GNUTLS_OID_X520_LOCALITY_NAME, 0, 0,
                                     dn_locality, &buflen) != 0)
    dn_locality[0] = '\0';
  buflen = sizeof (dn_province);
  if (gnutls_x509_crt_get_dn_by_oid (cert, GNUTLS_OID_X520_STATE_OR_PROVINCE_NAME, 0, 0,
                                     dn_province, &buflen) != 0)
    dn_province[0] = '\0';
  buflen = sizeof (dn_country);
  if (gnutls_x509_crt_get_dn_by_oid (cert, GNUTLS_OID_X520_COUNTRY_NAME, 0, 0,
                                     dn_country, &buflen) != 0)
    dn_country[0] = '\0';

  mutt_menu_add_dialog_row (menu, _("This certificate belongs to:"));
  mutt_buffer_printf (drow, "   %s  %s", dn_common_name, dn_email);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));
  mutt_buffer_printf (drow, "   %s", dn_organization);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));
  mutt_buffer_printf (drow, "   %s", dn_organizational_unit);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));
  mutt_buffer_printf (drow, "   %s  %s  %s",
            dn_locality, dn_province, dn_country);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));


  buflen = sizeof (dn_common_name);
  if (gnutls_x509_crt_get_issuer_dn_by_oid (cert, GNUTLS_OID_X520_COMMON_NAME, 0, 0,
                                            dn_common_name, &buflen) != 0)
    dn_common_name[0] = '\0';
  buflen = sizeof (dn_email);
  if (gnutls_x509_crt_get_issuer_dn_by_oid (cert, GNUTLS_OID_PKCS9_EMAIL, 0, 0,
                                            dn_email, &buflen) != 0)
    dn_email[0] = '\0';
  buflen = sizeof (dn_organization);
  if (gnutls_x509_crt_get_issuer_dn_by_oid (cert, GNUTLS_OID_X520_ORGANIZATION_NAME, 0, 0,
                                            dn_organization, &buflen) != 0)
    dn_organization[0] = '\0';
  buflen = sizeof (dn_organizational_unit);
  if (gnutls_x509_crt_get_issuer_dn_by_oid (cert, GNUTLS_OID_X520_ORGANIZATIONAL_UNIT_NAME, 0, 0,
                                            dn_organizational_unit, &buflen) != 0)
    dn_organizational_unit[0] = '\0';
  buflen = sizeof (dn_locality);
  if (gnutls_x509_crt_get_issuer_dn_by_oid (cert, GNUTLS_OID_X520_LOCALITY_NAME, 0, 0,
                                            dn_locality, &buflen) != 0)
    dn_locality[0] = '\0';
  buflen = sizeof (dn_province);
  if (gnutls_x509_crt_get_issuer_dn_by_oid (cert, GNUTLS_OID_X520_STATE_OR_PROVINCE_NAME, 0, 0,
                                            dn_province, &buflen) != 0)
    dn_province[0] = '\0';
  buflen = sizeof (dn_country);
  if (gnutls_x509_crt_get_issuer_dn_by_oid (cert, GNUTLS_OID_X520_COUNTRY_NAME, 0, 0,
                                            dn_country, &buflen) != 0)
    dn_country[0] = '\0';

  mutt_menu_add_dialog_row (menu, "");
  mutt_menu_add_dialog_row (menu, _("This certificate was issued by:"));
  mutt_buffer_printf (drow, "   %s  %s", dn_common_name, dn_email);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));
  mutt_buffer_printf (drow, "   %s", dn_organization);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));
  mutt_buffer_printf (drow, "   %s", dn_organizational_unit);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));
  mutt_buffer_printf (drow, "   %s  %s  %s",
            dn_locality, dn_province, dn_country);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));


  mutt_menu_add_dialog_row (menu, "");
  mutt_menu_add_dialog_row (menu, _("This certificate is valid"));

  t = gnutls_x509_crt_get_activation_time (cert);
  mutt_buffer_printf (drow, _("   from %s"),
	    tls_make_date (t, datestr, 30));
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));

  t = gnutls_x509_crt_get_expiration_time (cert);
  mutt_buffer_printf (drow, _("     to %s"),
	    tls_make_date (t, datestr, 30));
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));


  fpbuf[0] = '\0';
  tls_fingerprint (GNUTLS_DIG_SHA, fpbuf, sizeof (fpbuf), certdata);
  mutt_buffer_printf (drow, _("SHA1 Fingerprint: %s"), fpbuf);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));
  fpbuf[0] = '\0';
  fpbuf[40] = '\0';  /* Ensure the second printed line is null terminated */
  tls_fingerprint (GNUTLS_DIG_SHA256, fpbuf, sizeof (fpbuf), certdata);
  fpbuf[39] = '\0';  /* Divide into two lines of output */
  mutt_buffer_printf (drow, "%s%s", _("SHA256 Fingerprint: "), fpbuf);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));
  mutt_buffer_printf (drow, "%*s%s",
            (int)mutt_strlen (_("SHA256 Fingerprint: ")), "", fpbuf + 40);
  mutt_menu_add_dialog_row (menu, mutt_b2s (drow));


  if (certerr)
    mutt_menu_add_dialog_row (menu, "");
  if (certerr & CERTERR_NOTYETVALID)
    mutt_menu_add_dialog_row (menu, _("WARNING: Server certificate is not yet valid"));
  if (certerr & CERTERR_EXPIRED)
    mutt_menu_add_dialog_row (menu, _("WARNING: Server certificate has expired"));
  if (certerr & CERTERR_REVOKED)
    mutt_menu_add_dialog_row (menu, _("WARNING: Server certificate has been revoked"));
  if (certerr & CERTERR_HOSTNAME)
    mutt_menu_add_dialog_row (menu, _("WARNING: Server hostname does not match certificate"));
  if (certerr & CERTERR_SIGNERNOTCA)
    mutt_menu_add_dialog_row (menu, _("WARNING: Signer of server certificate is not a CA"));
  if (certerr & CERTERR_INSECUREALG)
    mutt_menu_add_dialog_row (menu,
                              _("Warning: Server certificate was signed using an insecure algorithm"));

  snprintf (title, sizeof (title),
            _("SSL Certificate check (certificate %d of %d in chain)"),
            len - idx, len);
  menu->title = title;
  /* certificates with bad dates, or that are revoked, must be
     accepted manually each and every time */
  if (SslCertFile && !savedcert
      && !(certerr & (CERTERR_EXPIRED | CERTERR_NOTYETVALID
                      | CERTERR_REVOKED)))
  {
    menu->prompt = _("(r)eject, accept (o)nce, (a)ccept always");
    /* L10N:
     * These three letters correspond to the choices in the string:
     * (r)eject, accept (o)nce, (a)ccept always.
     * This is an interactive certificate confirmation prompt for
     * a GNUTLS connection.
     */
    menu->keys = _("roa");
  }
  else
  {
    menu->prompt = _("(r)eject, accept (o)nce");
    /* L10N:
     * These two letters correspond to the choices in the string:
     * (r)eject, accept (o)nce.
     * These is an interactive certificate confirmation prompt for
     * a GNUTLS connection.
     */
    menu->keys = _("ro");
  }

  helpstr[0] = '\0';
  mutt_make_help (buf, sizeof (buf), _("Exit  "), MENU_GENERIC, OP_EXIT);
  safe_strcat (helpstr, sizeof (helpstr), buf);
  mutt_make_help (buf, sizeof (buf), _("Help"), MENU_GENERIC, OP_HELP);
  safe_strcat (helpstr, sizeof (helpstr), buf);
  menu->help = helpstr;

  done = 0;
  if (!option (OPTIGNOREMACROEVENTS))
  {
    set_option (OPTIGNOREMACROEVENTS);
    reset_ignoremacro = 1;
  }
  while (!done)
  {
    switch (mutt_menuLoop (menu))
    {
      case -1:			/* abort */
      case OP_MAX + 1:		/* reject */
      case OP_EXIT:
        done = 1;
        break;
      case OP_MAX + 3:		/* accept always */
        done = 0;
        if ((fp = fopen (SslCertFile, "a")))
	{
	  /* save hostname if necessary */
	  if (certerr & CERTERR_HOSTNAME)
	  {
            fpbuf[0] = '\0';
            tls_fingerprint (GNUTLS_DIG_MD5, fpbuf, sizeof (fpbuf), certdata);
	    fprintf (fp, "#H %s %s\n", hostname, fpbuf);
	    done = 1;
	  }
          /* Save the cert for all other errors */
	  if (certerr ^ CERTERR_HOSTNAME)
	  {
            done = 0;
	    ret = gnutls_pem_base64_encode_alloc ("CERTIFICATE", certdata,
                                                  &pemdata);
	    if (ret == 0)
	    {
	      if (fwrite (pemdata.data, pemdata.size, 1, fp) == 1)
	      {
		done = 1;
	      }
              gnutls_free (pemdata.data);
	    }
	  }
	  safe_fclose (&fp);
	}
	if (!done)
        {
	  mutt_error (_("Warning: Couldn't save certificate"));
	  mutt_sleep (2);
	}
	else
        {
	  mutt_message (_("Certificate saved"));
	  mutt_sleep (0);
	}
        /* fall through */
      case OP_MAX + 2:		/* accept once */
        done = 2;
        break;
    }
  }
  if (reset_ignoremacro)
    unset_option (OPTIGNOREMACROEVENTS);

  mutt_buffer_pool_release (&drow);
  mutt_pop_current_menu (menu);
  mutt_menuDestroy (&menu);
  gnutls_x509_crt_deinit (cert);

  return (done == 2) ? 1 : 0;
}

/* sanity-checking wrapper for gnutls_certificate_verify_peers.
 *
 * certstat is technically a bitwise-or of gnutls_certificate_status_t
 * values.
 *
 * Returns:
 *   - 0 if certstat was set. note: this does not mean success.
 *   - nonzero on failure.
 */
static int tls_verify_peers (gnutls_session_t tlsstate,
                             gnutls_certificate_status_t *certstat)
{
  int verify_ret;

  /* gnutls_certificate_verify_peers2() chains to
   * gnutls_x509_trust_list_verify_crt2().  That function's documentation says:
   *
   *   When a certificate chain of cert_list_size with more than one
   *   certificates is provided, the verification status will apply to
   *   the first certificate in the chain that failed
   *   verification. The verification process starts from the end of
   *   the chain (from CA to end certificate). The first certificate
   *   in the chain must be the end-certificate while the rest of the
   *   members may be sorted or not.
   *
   * This is why tls_check_certificate() loops from CA to host in that order,
   * calling the menu, and recalling tls_verify_peers() for each approved
   * cert in the chain.
   */
  verify_ret = gnutls_certificate_verify_peers2 (tlsstate, certstat);

  /* certstat was set */
  if (!verify_ret)
    return 0;

  if (verify_ret == GNUTLS_E_NO_CERTIFICATE_FOUND)
    mutt_error (_("Unable to get certificate from peer"));
  else
    mutt_error (_("Certificate verification error (%s)"),
                gnutls_strerror (verify_ret));

  mutt_sleep (2);
  return verify_ret;
}

/* Returns 1 on success.
 *         0 on failure.
 */
static int tls_check_certificate (CONNECTION* conn)
{
  tlssockdata *data = conn->sockdata;
  gnutls_session_t state = data->state;
  const gnutls_datum_t *cert_list;
  unsigned int cert_list_size = 0;
  gnutls_certificate_status_t certstat;
  int certerr, i, preauthrc, savedcert, rc = 0;
  int max_preauth_pass = -1;
  int rcsettrust;
  char *hostname;

  hostname = SslVerifyHostOverride ? SslVerifyHostOverride : conn->account.host;

  /* tls_verify_peers() calls gnutls_certificate_verify_peers2(),
   * which verifies the auth_type is GNUTLS_CRD_CERTIFICATE
   * and that get_certificate_type() for the server is GNUTLS_CRT_X509.
   * If it returns 0, certstat will be set with failure codes for the first
   * cert in the chain (from CA to host) with an error.
   */
  if (tls_verify_peers (state, &certstat) != 0)
    return 0;

  cert_list = gnutls_certificate_get_peers (state, &cert_list_size);
  if (!cert_list)
  {
    mutt_error (_("Unable to get certificate from peer"));
    mutt_sleep (2);
    return 0;
  }

  /* tls_verify_peers doesn't check hostname or expiration, so walk
   * from most specific to least checking these. If we see a saved certificate,
   * its status short-circuits the remaining checks. */
  preauthrc = 0;
  for (i = 0; i < cert_list_size; i++)
  {
    rc = tls_check_preauth (&cert_list[i], certstat, hostname, i,
                            &certerr, &savedcert);
    preauthrc += rc;
    if (!preauthrc)
      max_preauth_pass = i;

    if (savedcert)
    {
      if (!preauthrc)
        return 1;
      else
        break;
    }
  }

  /* then check interactively, starting from chain root */
  for (i = cert_list_size - 1; i >= 0; i--)
  {
    rc = tls_check_one_certificate (&cert_list[i], certstat, hostname,
                                    i, cert_list_size);

    /* Stop checking if the menu cert is aborted or rejected. */
    if (!rc)
      break;

    /* add signers to trust set, then reverify */
    if (i)
    {
      rcsettrust = gnutls_certificate_set_x509_trust_mem (data->xcred,
                                                          &cert_list[i],
                                                          GNUTLS_X509_FMT_DER);
      if (rcsettrust != 1)
        dprint (1, (debugfile, "error trusting certificate %d: %d\n", i, rcsettrust));

      if (tls_verify_peers (state, &certstat) != 0)
        return 0;

      /* If the cert chain now verifies, and all lower certs already
       * passed preauth, we are done.
       */
      if (!certstat && (max_preauth_pass >= i - 1))
        return 1;
    }
  }

  return rc;
}

static void client_cert_prompt (char *prompt, size_t prompt_size, ACCOUNT *account)
{
  /* L10N:
     When using a $ssl_client_cert, GNUTLS may prompt for the password
     to decrypt the cert.  %s is the hostname.
  */
  snprintf (prompt, prompt_size, _("Password for %s client cert: "),
            account->host);
}

static int tls_passwd_cb (void* userdata, int attempt, const char* token_url,
                          const char* token_label,
                          unsigned int flags,
                          char* buf, size_t size)
{
  ACCOUNT *account;

  if (!buf || size <= 0 || !userdata)
    return GNUTLS_E_INVALID_PASSWORD;

  account = (ACCOUNT *) userdata;

  if (_mutt_account_getpass (account, client_cert_prompt))
    return GNUTLS_E_INVALID_PASSWORD;

  snprintf(buf, size, "%s", account->pass);
  return GNUTLS_E_SUCCESS;
}
