/* Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* U2F implementation-specific callbacks and parameters. */

#ifndef __CROS_EC_U2F_IMPL_H
#define __CROS_EC_U2F_IMPL_H

#include "common.h"
#include "cryptoc/p256.h"
#include "u2f.h"

/* ---- Physical presence ---- */

enum touch_state {
	POP_TOUCH_NO   = 0,  /* waiting for a user touch */
	POP_TOUCH_YES  = 1,  /* touch recorded and latched */
};

/*
 * Check whether the user presence event was latched.
 *
 * @param consume reset the latched touch event and the presence LED.
 * @return POP_TOUCH_NO or POP_TOUCH_YES.
 */
enum touch_state pop_check_presence(int consume);

/* ---- platform cryptography hooks ---- */

/**
 * Pack the specified origin, user secret and origin-specific seed
 * into a key handle.
 *
 * @param origin pointer to origin id
 * @param user pointer to user secret
 * @param seed pointer to origin-specific random seed
 * @param key_handle buffer to hold the output key handle
 *
 * @return EC_SUCCESS if a valid keypair was created.
 */
int u2f_origin_user_keyhandle(const uint8_t *origin, const uint8_t *user,
			      const uint8_t *seed,
			      struct u2f_key_handle *key_handle);

/**
 * Pack the specified origin, user secret, origin-specific seed and version
 * byte into a key handle.
 *
 * @param origin pointer to origin id
 * @param user pointer to user secret
 * @param seed pointer to origin-specific random seed
 * @param version the version byte to pack; should be greater than 0.
 * @param key_handle_header buffer to hold the output key handle header
 *
 * @return EC_SUCCESS if a valid keypair was created.
 */
int u2f_origin_user_versioned_keyhandle(
	const uint8_t *origin, const uint8_t *user, const uint8_t *seed,
	uint8_t version,
	struct u2f_versioned_key_handle_header *key_handle_header);

/**
 * Generate an origin and user-specific ECDSA keypair from the specified
 * key handle.
 *
 * If pk_x and pk_y are NULL, public key generation will be skipped.
 *
 * @param key_handle pointer to the key handle
 * @param key_handle_size size of the key handle in bytes
 * @param d pointer to ECDSA private key
 * @param pk_x pointer to public key point
 * @param pk_y pointer to public key point
 *
 * @return EC_SUCCESS if a valid keypair was created.
 */
int u2f_origin_user_keypair(const uint8_t *key_handle, size_t key_handle_size,
			    p256_int *d, p256_int *pk_x, p256_int *pk_y);

/**
 * Derive an hmac from the given salt, key handle and hash. The salt is to make
 * sure the hmac is different for different key handles of one user. The key
 * handle header is encoded into the authorization hmac to protect against
 * swapping auth time secret.
 */
int u2f_authorization_hmac(const uint8_t *authorization_salt,
			   const struct u2f_versioned_key_handle_header *header,
			   const uint8_t *auth_time_secret_hash, uint8_t *hmac);

/***
 * Generate a hardware derived 256b private key.
 *
 * @param kek ptr to store the generated key.
 * @param key_len size of the storage buffer. Should be 32 bytes.
 * @return EC_SUCCESS if a valid key was created.
 */
int u2f_gen_kek(const uint8_t *origin, uint8_t *kek, size_t key_len);

/**
 * Generate a hardware derived ECDSA keypair for individual attestation.
 *
 * @param seed ptr to store 32-byte seed to regenerate this key on this chip
 * @param d pointer to ECDSA private key
 * @param pk_x pointer to public key point
 * @param pk_y pointer to public key point
 *
 * @return EC_SUCCESS if a valid keypair was created.
 */
int g2f_individual_keypair(p256_int *d, p256_int *pk_x, p256_int *pk_y);

/***
 * Generates and persists to nvram a new seed that will be used to
 * derive kek in future calls to u2f_gen_kek().
 *
 * @param commit whether to commit nvram changes before returning.
 * @return EC_SUCCESS if seed was successfully created
 * (and persisted if requested).
 */
int u2f_gen_kek_seed(int commit);

/* Maximum size in bytes of G2F attestation certificate. */
#define G2F_ATTESTATION_CERT_MAX_LEN	315

/**
 * Gets the x509 certificate for the attestation keypair returned
 * by g2f_individual_keypair().
 *
 * @param buf pointer to a buffer that must be at least
 * G2F_ATTESTATION_CERT_MAX_LEN bytes.
 * @return size of certificate written to buf, 0 on error.
 */
int g2f_attestation_cert(uint8_t *buf);

#endif /* __CROS_EC_U2F_IMPL_H */
