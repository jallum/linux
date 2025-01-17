// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The AEGIS-256 Authenticated-Encryption Algorithm
 *
 * Copyright (c) 2017-2018 Ondrej Mosnacek <omosnacek@gmail.com>
 * Copyright (C) 2017-2018 Red Hat, Inc. All rights reserved.
 */

#include <crypto/algapi.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/scatterlist.h>

#include "aegis.h"

#define AEGIS256_NONCE_SIZE 32
#define AEGIS256_STATE_BLOCKS 6
#define AEGIS256_KEY_SIZE 32
#define AEGIS256_MIN_AUTH_SIZE 8
#define AEGIS256_MAX_AUTH_SIZE 16

struct aegis_state {
	union aegis_block blocks[AEGIS256_STATE_BLOCKS];
};

struct aegis_ctx {
	union aegis_block key[AEGIS256_KEY_SIZE / AEGIS_BLOCK_SIZE];
};

struct aegis256_ops {
	int (*skcipher_walk_init)(struct skcipher_walk *walk,
				  struct aead_request *req, bool atomic);

	void (*crypt_chunk)(struct aegis_state *state, u8 *dst,
			    const u8 *src, unsigned int size);
};

static void crypto_aegis256_update(struct aegis_state *state)
{
	union aegis_block tmp;
	unsigned int i;

	tmp = state->blocks[AEGIS256_STATE_BLOCKS - 1];
	for (i = AEGIS256_STATE_BLOCKS - 1; i > 0; i--)
		crypto_aegis_aesenc(&state->blocks[i], &state->blocks[i - 1],
				    &state->blocks[i]);
	crypto_aegis_aesenc(&state->blocks[0], &tmp, &state->blocks[0]);
}

static void crypto_aegis256_update_a(struct aegis_state *state,
				     const union aegis_block *msg)
{
	crypto_aegis256_update(state);
	crypto_aegis_block_xor(&state->blocks[0], msg);
}

static void crypto_aegis256_update_u(struct aegis_state *state, const void *msg)
{
	crypto_aegis256_update(state);
	crypto_xor(state->blocks[0].bytes, msg, AEGIS_BLOCK_SIZE);
}

static void crypto_aegis256_init(struct aegis_state *state,
				 const union aegis_block *key,
				 const u8 *iv)
{
	union aegis_block key_iv[2];
	unsigned int i;

	key_iv[0] = key[0];
	key_iv[1] = key[1];
	crypto_xor(key_iv[0].bytes, iv + 0 * AEGIS_BLOCK_SIZE,
			AEGIS_BLOCK_SIZE);
	crypto_xor(key_iv[1].bytes, iv + 1 * AEGIS_BLOCK_SIZE,
			AEGIS_BLOCK_SIZE);

	state->blocks[0] = key_iv[0];
	state->blocks[1] = key_iv[1];
	state->blocks[2] = crypto_aegis_const[1];
	state->blocks[3] = crypto_aegis_const[0];
	state->blocks[4] = key[0];
	state->blocks[5] = key[1];

	crypto_aegis_block_xor(&state->blocks[4], &crypto_aegis_const[0]);
	crypto_aegis_block_xor(&state->blocks[5], &crypto_aegis_const[1]);

	for (i = 0; i < 4; i++) {
		crypto_aegis256_update_a(state, &key[0]);
		crypto_aegis256_update_a(state, &key[1]);
		crypto_aegis256_update_a(state, &key_iv[0]);
		crypto_aegis256_update_a(state, &key_iv[1]);
	}
}

static void crypto_aegis256_ad(struct aegis_state *state,
			       const u8 *src, unsigned int size)
{
	if (AEGIS_ALIGNED(src)) {
		const union aegis_block *src_blk =
				(const union aegis_block *)src;

		while (size >= AEGIS_BLOCK_SIZE) {
			crypto_aegis256_update_a(state, src_blk);

			size -= AEGIS_BLOCK_SIZE;
			src_blk++;
		}
	} else {
		while (size >= AEGIS_BLOCK_SIZE) {
			crypto_aegis256_update_u(state, src);

			size -= AEGIS_BLOCK_SIZE;
			src += AEGIS_BLOCK_SIZE;
		}
	}
}

static void crypto_aegis256_encrypt_chunk(struct aegis_state *state, u8 *dst,
					  const u8 *src, unsigned int size)
{
	union aegis_block tmp;

	if (AEGIS_ALIGNED(src) && AEGIS_ALIGNED(dst)) {
		while (size >= AEGIS_BLOCK_SIZE) {
			union aegis_block *dst_blk =
					(union aegis_block *)dst;
			const union aegis_block *src_blk =
					(const union aegis_block *)src;

			tmp = state->blocks[2];
			crypto_aegis_block_and(&tmp, &state->blocks[3]);
			crypto_aegis_block_xor(&tmp, &state->blocks[5]);
			crypto_aegis_block_xor(&tmp, &state->blocks[4]);
			crypto_aegis_block_xor(&tmp, &state->blocks[1]);
			crypto_aegis_block_xor(&tmp, src_blk);

			crypto_aegis256_update_a(state, src_blk);

			*dst_blk = tmp;

			size -= AEGIS_BLOCK_SIZE;
			src += AEGIS_BLOCK_SIZE;
			dst += AEGIS_BLOCK_SIZE;
		}
	} else {
		while (size >= AEGIS_BLOCK_SIZE) {
			tmp = state->blocks[2];
			crypto_aegis_block_and(&tmp, &state->blocks[3]);
			crypto_aegis_block_xor(&tmp, &state->blocks[5]);
			crypto_aegis_block_xor(&tmp, &state->blocks[4]);
			crypto_aegis_block_xor(&tmp, &state->blocks[1]);
			crypto_xor(tmp.bytes, src, AEGIS_BLOCK_SIZE);

			crypto_aegis256_update_u(state, src);

			memcpy(dst, tmp.bytes, AEGIS_BLOCK_SIZE);

			size -= AEGIS_BLOCK_SIZE;
			src += AEGIS_BLOCK_SIZE;
			dst += AEGIS_BLOCK_SIZE;
		}
	}

	if (size > 0) {
		union aegis_block msg = {};
		memcpy(msg.bytes, src, size);

		tmp = state->blocks[2];
		crypto_aegis_block_and(&tmp, &state->blocks[3]);
		crypto_aegis_block_xor(&tmp, &state->blocks[5]);
		crypto_aegis_block_xor(&tmp, &state->blocks[4]);
		crypto_aegis_block_xor(&tmp, &state->blocks[1]);

		crypto_aegis256_update_a(state, &msg);

		crypto_aegis_block_xor(&msg, &tmp);

		memcpy(dst, msg.bytes, size);
	}
}

static void crypto_aegis256_decrypt_chunk(struct aegis_state *state, u8 *dst,
					  const u8 *src, unsigned int size)
{
	union aegis_block tmp;

	if (AEGIS_ALIGNED(src) && AEGIS_ALIGNED(dst)) {
		while (size >= AEGIS_BLOCK_SIZE) {
			union aegis_block *dst_blk =
					(union aegis_block *)dst;
			const union aegis_block *src_blk =
					(const union aegis_block *)src;

			tmp = state->blocks[2];
			crypto_aegis_block_and(&tmp, &state->blocks[3]);
			crypto_aegis_block_xor(&tmp, &state->blocks[5]);
			crypto_aegis_block_xor(&tmp, &state->blocks[4]);
			crypto_aegis_block_xor(&tmp, &state->blocks[1]);
			crypto_aegis_block_xor(&tmp, src_blk);

			crypto_aegis256_update_a(state, &tmp);

			*dst_blk = tmp;

			size -= AEGIS_BLOCK_SIZE;
			src += AEGIS_BLOCK_SIZE;
			dst += AEGIS_BLOCK_SIZE;
		}
	} else {
		while (size >= AEGIS_BLOCK_SIZE) {
			tmp = state->blocks[2];
			crypto_aegis_block_and(&tmp, &state->blocks[3]);
			crypto_aegis_block_xor(&tmp, &state->blocks[5]);
			crypto_aegis_block_xor(&tmp, &state->blocks[4]);
			crypto_aegis_block_xor(&tmp, &state->blocks[1]);
			crypto_xor(tmp.bytes, src, AEGIS_BLOCK_SIZE);

			crypto_aegis256_update_a(state, &tmp);

			memcpy(dst, tmp.bytes, AEGIS_BLOCK_SIZE);

			size -= AEGIS_BLOCK_SIZE;
			src += AEGIS_BLOCK_SIZE;
			dst += AEGIS_BLOCK_SIZE;
		}
	}

	if (size > 0) {
		union aegis_block msg = {};
		memcpy(msg.bytes, src, size);

		tmp = state->blocks[2];
		crypto_aegis_block_and(&tmp, &state->blocks[3]);
		crypto_aegis_block_xor(&tmp, &state->blocks[5]);
		crypto_aegis_block_xor(&tmp, &state->blocks[4]);
		crypto_aegis_block_xor(&tmp, &state->blocks[1]);
		crypto_aegis_block_xor(&msg, &tmp);

		memset(msg.bytes + size, 0, AEGIS_BLOCK_SIZE - size);

		crypto_aegis256_update_a(state, &msg);

		memcpy(dst, msg.bytes, size);
	}
}

static void crypto_aegis256_process_ad(struct aegis_state *state,
				       struct scatterlist *sg_src,
				       unsigned int assoclen)
{
	struct scatter_walk walk;
	union aegis_block buf;
	unsigned int pos = 0;

	scatterwalk_start(&walk, sg_src);
	while (assoclen != 0) {
		unsigned int size = scatterwalk_clamp(&walk, assoclen);
		unsigned int left = size;
		void *mapped = scatterwalk_map(&walk);
		const u8 *src = (const u8 *)mapped;

		if (pos + size >= AEGIS_BLOCK_SIZE) {
			if (pos > 0) {
				unsigned int fill = AEGIS_BLOCK_SIZE - pos;
				memcpy(buf.bytes + pos, src, fill);
				crypto_aegis256_update_a(state, &buf);
				pos = 0;
				left -= fill;
				src += fill;
			}

			crypto_aegis256_ad(state, src, left);
			src += left & ~(AEGIS_BLOCK_SIZE - 1);
			left &= AEGIS_BLOCK_SIZE - 1;
		}

		memcpy(buf.bytes + pos, src, left);

		pos += left;
		assoclen -= size;
		scatterwalk_unmap(mapped);
		scatterwalk_advance(&walk, size);
		scatterwalk_done(&walk, 0, assoclen);
	}

	if (pos > 0) {
		memset(buf.bytes + pos, 0, AEGIS_BLOCK_SIZE - pos);
		crypto_aegis256_update_a(state, &buf);
	}
}

static void crypto_aegis256_process_crypt(struct aegis_state *state,
					  struct aead_request *req,
					  const struct aegis256_ops *ops)
{
	struct skcipher_walk walk;
	u8 *src, *dst;
	unsigned int chunksize;

	ops->skcipher_walk_init(&walk, req, false);

	while (walk.nbytes) {
		src = walk.src.virt.addr;
		dst = walk.dst.virt.addr;
		chunksize = walk.nbytes;

		ops->crypt_chunk(state, dst, src, chunksize);

		skcipher_walk_done(&walk, 0);
	}
}

static void crypto_aegis256_final(struct aegis_state *state,
				  union aegis_block *tag_xor,
				  u64 assoclen, u64 cryptlen)
{
	u64 assocbits = assoclen * 8;
	u64 cryptbits = cryptlen * 8;

	union aegis_block tmp;
	unsigned int i;

	tmp.words64[0] = cpu_to_le64(assocbits);
	tmp.words64[1] = cpu_to_le64(cryptbits);

	crypto_aegis_block_xor(&tmp, &state->blocks[3]);

	for (i = 0; i < 7; i++)
		crypto_aegis256_update_a(state, &tmp);

	for (i = 0; i < AEGIS256_STATE_BLOCKS; i++)
		crypto_aegis_block_xor(tag_xor, &state->blocks[i]);
}

static int crypto_aegis256_setkey(struct crypto_aead *aead, const u8 *key,
				  unsigned int keylen)
{
	struct aegis_ctx *ctx = crypto_aead_ctx(aead);

	if (keylen != AEGIS256_KEY_SIZE) {
		crypto_aead_set_flags(aead, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	}

	memcpy(ctx->key[0].bytes, key, AEGIS_BLOCK_SIZE);
	memcpy(ctx->key[1].bytes, key + AEGIS_BLOCK_SIZE,
			AEGIS_BLOCK_SIZE);
	return 0;
}

static int crypto_aegis256_setauthsize(struct crypto_aead *tfm,
				       unsigned int authsize)
{
	if (authsize > AEGIS256_MAX_AUTH_SIZE)
		return -EINVAL;
	if (authsize < AEGIS256_MIN_AUTH_SIZE)
		return -EINVAL;
	return 0;
}

static void crypto_aegis256_crypt(struct aead_request *req,
				  union aegis_block *tag_xor,
				  unsigned int cryptlen,
				  const struct aegis256_ops *ops)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct aegis_ctx *ctx = crypto_aead_ctx(tfm);
	struct aegis_state state;

	crypto_aegis256_init(&state, ctx->key, req->iv);
	crypto_aegis256_process_ad(&state, req->src, req->assoclen);
	crypto_aegis256_process_crypt(&state, req, ops);
	crypto_aegis256_final(&state, tag_xor, req->assoclen, cryptlen);
}

static int crypto_aegis256_encrypt(struct aead_request *req)
{
	static const struct aegis256_ops ops = {
		.skcipher_walk_init = skcipher_walk_aead_encrypt,
		.crypt_chunk = crypto_aegis256_encrypt_chunk,
	};

	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	union aegis_block tag = {};
	unsigned int authsize = crypto_aead_authsize(tfm);
	unsigned int cryptlen = req->cryptlen;

	crypto_aegis256_crypt(req, &tag, cryptlen, &ops);

	scatterwalk_map_and_copy(tag.bytes, req->dst, req->assoclen + cryptlen,
				 authsize, 1);
	return 0;
}

static int crypto_aegis256_decrypt(struct aead_request *req)
{
	static const struct aegis256_ops ops = {
		.skcipher_walk_init = skcipher_walk_aead_decrypt,
		.crypt_chunk = crypto_aegis256_decrypt_chunk,
	};
	static const u8 zeros[AEGIS256_MAX_AUTH_SIZE] = {};

	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	union aegis_block tag;
	unsigned int authsize = crypto_aead_authsize(tfm);
	unsigned int cryptlen = req->cryptlen - authsize;

	scatterwalk_map_and_copy(tag.bytes, req->src, req->assoclen + cryptlen,
				 authsize, 0);

	crypto_aegis256_crypt(req, &tag, cryptlen, &ops);

	return crypto_memneq(tag.bytes, zeros, authsize) ? -EBADMSG : 0;
}

static int crypto_aegis256_init_tfm(struct crypto_aead *tfm)
{
	return 0;
}

static void crypto_aegis256_exit_tfm(struct crypto_aead *tfm)
{
}

static struct aead_alg crypto_aegis256_alg = {
	.setkey = crypto_aegis256_setkey,
	.setauthsize = crypto_aegis256_setauthsize,
	.encrypt = crypto_aegis256_encrypt,
	.decrypt = crypto_aegis256_decrypt,
	.init = crypto_aegis256_init_tfm,
	.exit = crypto_aegis256_exit_tfm,

	.ivsize = AEGIS256_NONCE_SIZE,
	.maxauthsize = AEGIS256_MAX_AUTH_SIZE,
	.chunksize = AEGIS_BLOCK_SIZE,

	.base = {
		.cra_blocksize = 1,
		.cra_ctxsize = sizeof(struct aegis_ctx),
		.cra_alignmask = 0,

		.cra_priority = 100,

		.cra_name = "aegis256",
		.cra_driver_name = "aegis256-generic",

		.cra_module = THIS_MODULE,
	}
};

static int __init crypto_aegis256_module_init(void)
{
	return crypto_register_aead(&crypto_aegis256_alg);
}

static void __exit crypto_aegis256_module_exit(void)
{
	crypto_unregister_aead(&crypto_aegis256_alg);
}

module_init(crypto_aegis256_module_init);
module_exit(crypto_aegis256_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ondrej Mosnacek <omosnacek@gmail.com>");
MODULE_DESCRIPTION("AEGIS-256 AEAD algorithm");
MODULE_ALIAS_CRYPTO("aegis256");
MODULE_ALIAS_CRYPTO("aegis256-generic");
