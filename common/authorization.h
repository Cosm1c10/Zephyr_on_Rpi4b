#ifndef AUTHORIZATION_H
#define AUTHORIZATION_H

#include <openssl/ssl.h>

typedef enum {
    ROLE_VIEWER = 0,
    ROLE_OPERATOR,
    ROLE_MAINTENANCE,
    ROLE_ADMIN,
    ROLE_UNAUTHORIZED
} UserRole;

typedef struct {
    char common_name[64];
    UserRole role;
} ClientIdentity;

/**
 * authorize_client: Extracts identity info from the mTLS certificate.
 * Returns 0 on success, -1 on failure.
 */
int authorize_client(SSL *ssl, ClientIdentity *out_id);

/**
 * role_to_string: Converts enum to readable text.
 */
const char* role_to_string(UserRole role);

#endif // AUTHORIZATION_H
