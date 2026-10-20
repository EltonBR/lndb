#ifndef LNDB_CLIENT_SHELL_H
#define LNDB_CLIENT_SHELL_H

/* Run a shell operation over an authenticated LNDB connection.
 * argc == 0 selects the interactive or stdin-script mode. */
int lndb_client_shell_run(int fd, int argc, char **argv);

#endif
