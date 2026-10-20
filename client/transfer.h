#ifndef LNDB_CLIENT_TRANSFER_H
#define LNDB_CLIENT_TRANSFER_H
/* Returns 0 on success and -1 on local, transport, or remote failure.
   Remote FILE_DONE status values are never returned directly. */
int lndb_client_transfer(int fd, int push, const char *src, const char *dst);
#endif
