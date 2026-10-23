#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*fsnet_handler)(uint64_t request, const char* bytes, size_t length);
typedef void (*fsnet_completion)(int status, const char* bytes, size_t length);
const char* fsnet_last_error(void);
void* fsnet_create(const char* token);
void* fsnet_create_tls(const char* token, const char* ca, const char* certificate, const char* key, const char* peer_name);
typedef int (*fsnet_authorizer)(uint64_t peer, const char* identity, const char* route);
int fsnet_authorize(void* node, fsnet_authorizer callback);
void fsnet_destroy(void* node);
int fsnet_listen(void* node, const char* address, int port);
int64_t fsnet_connect(void* node, const char* address, int port);
int fsnet_bind(void* node, const char* route, fsnet_handler callback);
int fsnet_respond(void* node, uint64_t request, int status, const char* bytes, size_t length);
int fsnet_call(void* node, uint64_t peer, const char* route, const char* bytes, size_t length,
               int timeout_ms, fsnet_completion callback);
int fsnet_poll(void* node);
void* fsnet_db_open(const char* filename); /* empty filename => memory */
void fsnet_db_close(void* database);
char* fsnet_db_get(void* database, const char* key, size_t key_length, size_t* length, uint64_t* version);
int fsnet_db_cas(void* database, const char* key, size_t key_length, uint64_t version, const char* bytes, size_t length);
int fsnet_db_erase(void* database, const char* key, size_t key_length, uint64_t version);
int fsnet_db_fenced_cas(void* database, const char* guard, size_t guard_length, uint64_t fence,
                        const char* key, size_t key_length, uint64_t version, const char* bytes, size_t length);
void fsnet_free(void* bytes);
#ifdef __cplusplus
}
#endif
