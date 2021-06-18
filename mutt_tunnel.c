/*
 * Copyright (C) 2000 Manoj Kasichainula <manoj@io.com>
 * Copyright (C) 2001,2005 Brendan Cully <brendan@kublai.com>
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

#include "mutt.h"
#include "mutt_socket.h"
#include "mutt_tunnel.h"

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

/* -- data types -- */
typedef struct
{
  pid_t pid;
  int readfd;
  int writefd;
} TUNNEL_DATA;

/* forward declarations */
static int tunnel_socket_open (CONNECTION*);
static int tunnel_socket_close (CONNECTION*);
static int tunnel_socket_read (CONNECTION* conn, char* buf, size_t len);
static int tunnel_socket_write (CONNECTION* conn, const char* buf, size_t len);
static int tunnel_socket_poll (CONNECTION* conn, time_t wait_secs);

/* -- public functions -- */
int mutt_tunnel_socket_setup (CONNECTION *conn)
{
  conn->conn_open = tunnel_socket_open;
  conn->conn_close = tunnel_socket_close;
  conn->conn_read = tunnel_socket_read;
  conn->conn_write = tunnel_socket_write;
  conn->conn_poll = tunnel_socket_poll;

  /* Note we are using ssf as a boolean in this case.  See the notes
   * in mutt_socket.h */
  if (option (OPTTUNNELISSECURE))
    conn->ssf = 1;

  return 0;
}

static int tunnel_socket_open (CONNECTION *conn)
{
  TUNNEL_DATA* tunnel;
  int pid;
  int rc;
  int pin[2], pout[2];
  int devnull;

  tunnel = (TUNNEL_DATA*) safe_malloc (sizeof (TUNNEL_DATA));
  conn->sockdata = tunnel;

  mutt_message (_("Connecting with \"%s\"..."), Tunnel);

  if ((rc = pipe (pin)) == -1)
  {
    mutt_perror ("pipe");
    FREE (&conn->sockdata);
    return -1;
  }
  if ((rc = pipe (pout)) == -1)
  {
    mutt_perror ("pipe");
    close (pin[0]);
    close (pin[1]);
    FREE (&conn->sockdata);
    return -1;
  }

  mutt_block_signals_system ();
  if ((pid = fork ()) == 0)
  {
    mutt_unblock_signals_system (0);
    mutt_reset_child_signals ();
    devnull = open ("/dev/null", O_RDWR);
    if (devnull < 0 ||
        dup2 (pout[0], STDIN_FILENO) < 0 ||
        dup2 (pin[1], STDOUT_FILENO) < 0 ||
        dup2 (devnull, STDERR_FILENO) < 0)
      _exit (127);
    close (pin[0]);
    close (pin[1]);
    close (pout[0]);
    close (pout[1]);
    close (devnull);

    /* Don't let the subprocess think it can use the controlling tty */
    setsid ();

    execle (EXECSHELL, "sh", "-c", Tunnel, NULL, mutt_envlist ());
    _exit (127);
  }
  mutt_unblock_signals_system (1);

  if (pid == -1)
  {
    mutt_perror ("fork");
    close (pin[0]);
    close (pin[1]);
    close (pout[0]);
    close (pout[1]);
    FREE (&conn->sockdata);
    return -1;
  }
  if (close (pin[1]) < 0 || close (pout[0]) < 0)
    mutt_perror ("close");

  fcntl (pin[0], F_SETFD, FD_CLOEXEC);
  fcntl (pout[1], F_SETFD, FD_CLOEXEC);

  tunnel->readfd = pin[0];
  tunnel->writefd = pout[1];
  tunnel->pid = pid;

  conn->fd = 42; /* stupid hack */

  return 0;
}

static int tunnel_socket_close (CONNECTION* conn)
{
  TUNNEL_DATA* tunnel = (TUNNEL_DATA*) conn->sockdata;
  int status;

  close (tunnel->readfd);
  close (tunnel->writefd);
  waitpid (tunnel->pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status))
  {
    mutt_error(_("Tunnel to %s returned error %d (%s)"), conn->account.host,
               WEXITSTATUS(status),
               NONULL(mutt_strsysexit(WEXITSTATUS(status))));
    mutt_sleep (2);
  }
  FREE (&conn->sockdata);

  return 0;
}

static int tunnel_socket_read (CONNECTION* conn, char* buf, size_t len)
{
  TUNNEL_DATA* tunnel = (TUNNEL_DATA*) conn->sockdata;
  int rc;

  do
  {
    rc = read (tunnel->readfd, buf, len);
  } while (rc < 0 && errno == EINTR);

  if (rc < 0)
  {
    mutt_error (_("Tunnel error talking to %s: %s"), conn->account.host,
		strerror (errno));
    mutt_sleep (1);
    return -1;
  }

  return rc;
}

static int tunnel_socket_write (CONNECTION* conn, const char* buf, size_t len)
{
  TUNNEL_DATA* tunnel = (TUNNEL_DATA*) conn->sockdata;
  int rc;
  size_t sent = 0;

  do
  {
    do
    {
      rc = write (tunnel->writefd, buf + sent, len - sent);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0)
    {
      mutt_error (_("Tunnel error talking to %s: %s"), conn->account.host,
                  strerror (errno));
      mutt_sleep (1);
      return -1;
    }

    sent += rc;
  } while (sent < len);

  return sent;
}

static int tunnel_socket_poll (CONNECTION* conn, time_t wait_secs)
{
  TUNNEL_DATA* tunnel = (TUNNEL_DATA*) conn->sockdata;
  int ofd;
  int rc;

  ofd = conn->fd;
  conn->fd = tunnel->readfd;
  rc = raw_socket_poll (conn, wait_secs);
  conn->fd = ofd;

  return rc;
}
