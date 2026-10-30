#ifndef DVBLAST_WEB_SERVER_H
#define DVBLAST_WEB_SERVER_H

#include <stdbool.h>

bool web_server_start(int port, const char *host);
void web_server_stop(void);
bool web_server_is_running(void);

#endif /* DVBLAST_WEB_SERVER_H */
