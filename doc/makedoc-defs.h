/* build complete documentation */

#ifdef MAKEDOC_FULL
# ifndef CRYPT_BACKEND_GPGME
#  define CRYPT_BACKEND_GPGME
# endif
# ifndef USE_IMAP
#  define USE_IMAP
# endif
# ifndef MIXMASTER
#  define MIXMASTER "mixmaster"
# endif
# ifndef USE_POP
#  define USE_POP
# endif
# ifndef USE_SMTP
#  define USE_SMTP
# endif
# ifndef USE_SSL_OPENSSL
#  define USE_SSL_OPENSSL
# endif
# ifndef HAVE_SSL_PARTIAL_CHAIN
#  define HAVE_SSL_PARTIAL_CHAIN
# endif
# ifndef USE_SSL_GNUTLS
#  define USE_SSL_GNUTLS
# endif
# ifndef USE_SSL
#  define USE_SSL
# endif
# ifndef USE_SOCKET
#  define USE_SOCKET
# endif
# ifndef USE_DOTLOCK
#  define USE_DOTLOCK
# endif
# ifndef DL_STANDALONE
#  define DL_STANDALONE
# endif
# ifndef USE_HCACHE
#  define USE_HCACHE
# endif
# ifndef HAVE_DB4
#  define HAVE_DB4
# endif
# ifndef HAVE_GDBM
#  define HAVE_GDBM
# endif
# ifndef HAVE_QDBM
#  define HAVE_QDBM
# endif
# ifndef HAVE_LIBIDN
#  define HAVE_LIBIDN
# endif
# ifndef HAVE_LIBIDN2
#  define HAVE_LIBIDN2
# endif
# ifndef HAVE_GETADDRINFO
#  define HAVE_GETADDRINFO
# endif
# ifndef USE_SASL
#  define USE_SASL
# endif
# ifndef USE_SASL_CYRUS
#  define USE_SASL_CYRUS
# endif
# ifndef USE_SASL_GNU
#  define USE_SASL_GNU
# endif
# ifndef USE_SIDEBAR
#  define USE_SIDEBAR
# endif
# ifndef USE_COMPRESSED
#  define USE_COMPRESSED
# endif
# ifndef USE_AUTOCRYPT
#  define USE_AUTOCRYPT
# endif
# ifndef USE_ZLIB
#  define USE_ZLIB
# endif
#endif
