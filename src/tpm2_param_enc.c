/* tpm2_param_enc.c
 *
 * Copyright (C) 2006-2020 wolfSSL Inc.
 *
 * This file is part of wolfTPM.
 *
 * wolfTPM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfTPM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

#include <wolftpm/tpm2_param_enc.h>
#include <wolftpm/tpm2_packet.h>

#ifndef WOLFTPM2_NO_WOLFCRYPT
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/hmac.h>
#endif

/* Routines for performing TPM Parameter Encryption
 *
 * NB: Only TPM2B_DATA parameters can be encrypted
 *
 * Only the first parameter of a TPM command can be encrypted.
 * For example, the password auth of a TPM key. The encryption
 * of command response and request are separate. There can be a
 * communication exchange between the TPM and a client program
 * where only the parameter in the request command is encrypted.
 *
 * This behavior depends on the sessionAttributes:
 * - TPMA_SESSION_encrypt for command request
 * - TPMA_SESSION_decrypt for command response
 * Either one can be set separately or both can be set in one
 * authorization session. This is up to the user(developer).
 *
 */

/******************************************************************************/
/* --- Local Functions -- */
/******************************************************************************/

/* This function performs key generation according to Part 1 of the TPM spec
 * and returns the number of bytes generated, which may be zero.
 *
 * 'key' input data is used together with the label, ContextU and ContextV to
 * generate the session key.
 *
 * 'keyStream' points to the buffer storing the generated session key, and
 * 'keyStream' can not be NULL.
 *
 * 'sizeInBits' must be no larger than (2^18)-1 = 256K bits (32385 bytes).
 *
 * Note: The "once" parameter is set to allow incremental generation of a large
 * value. If this flag is TRUE, "sizeInBits" is used in the HMAC computation
 * but only one iteration of the KDF is performed. This would be used for
 * XOR obfuscation so that the mask value can be generated in digest-sized
 * chunks rather than having to be generated all at once in an arbitrarily
 * large buffer and then XORed into the result. If "once" is TRUE, then
 * "sizeInBits" must be a multiple of 8.
 *
 * Any error in the processing of this command is considered fatal.
 *
 * Return values:
 *     0    hash algorithm is not supported or is TPM_ALG_NULL
 *    >0    the number of bytes in the 'key' buffer
 *
 */
int TPM2_KDFa(
    TPM_ALG_ID   hashAlg,   /* IN: hash algorithm used in HMAC */
    TPM2B_DATA  *keyIn,     /* IN: key */
    const char  *label,     /* IN: a 0-byte terminated label used in KDF */
    TPM2B_NONCE *contextU,  /* IN: context U (newer) */
    TPM2B_NONCE *contextV,  /* IN: context V */
    BYTE        *key,       /* OUT: key buffer */
    UINT32       keySz      /* IN: size of generated key in bytes */
)
{
#ifndef WOLFTPM2_NO_WOLFCRYPT
    int ret, hashType;
    Hmac hmac_ctx;
    word32 counter = 0;
    int hLen, lLen = 0;
    byte uint32Buf[sizeof(UINT32)];;
    UINT32 sizeInBits = keySz * 8, pos;
    BYTE* keyStream = key;

    if (key == NULL || keyStream == NULL)
        return BAD_FUNC_ARG;

    hashType = TPM2_GetHashType(hashAlg);
    if (hashType == WC_HASH_TYPE_NONE)
        return NOT_COMPILED_IN;

    hLen = TPM2_GetHashDigestSize(hashAlg);
    if (hLen <= 0)
        return NOT_COMPILED_IN;

    /* get label length if provided, including null termination */
    if (label != NULL) {
        lLen = (int)XSTRLEN(label) + 1;
    }

    ret = wc_HmacInit(&hmac_ctx, NULL, INVALID_DEVID);
    if (ret != 0)
        return ret;

    /* generate required bytes - blocks sized digest */
    for (pos = 0; pos < keySz; pos += hLen) {
        /* KDFa counter starts at 1 */
        counter++;

        /* start HMAC */
        ret = wc_HmacSetKey(&hmac_ctx, hashType, keyIn->buffer, keyIn->size);
        if (ret != 0)
            goto exit;

        /* add counter - KDFa i2 */
        TPM2_Packet_U32ToByteArray(counter, uint32Buf);
        ret = wc_HmacUpdate(&hmac_ctx, uint32Buf, (word32)sizeof(uint32Buf));
        if (ret != 0)
            goto exit;

        /* add label - KDFa label */
        if (label != NULL) {
            ret = wc_HmacUpdate(&hmac_ctx, (byte*)label, lLen);
            if (ret != 0)
                goto exit;
        }

        /* add contextU */
        if (contextU != NULL && contextU->size > 0) {
            ret = wc_HmacUpdate(&hmac_ctx, contextU->buffer, contextU->size);
            if (ret != 0)
                goto exit;
        }

        /* add contextV */
        if (contextV != NULL && contextV->size > 0) {
            ret = wc_HmacUpdate(&hmac_ctx, contextV->buffer, contextV->size);
            if (ret != 0)
                goto exit;
        }

        /* add size in bits */
        TPM2_Packet_U32ToByteArray(sizeInBits, uint32Buf);
        ret = wc_HmacUpdate(&hmac_ctx, uint32Buf, (word32)sizeof(uint32Buf));
        if (ret != 0)
            goto exit;

        /* get result */
        ret = wc_HmacFinal(&hmac_ctx, keyStream);
        if (ret != 0)
            goto exit;

        keyStream += hLen;
    }
    ret = keySz;

exit:
    wc_HmacFree(&hmac_ctx);

    /* return length rounded up to nearest 8 multiple */
    return ret;
#else
    (void)hashAlg;
    (void)keyIn;
    (void)label;
    (void)contextU;
    (void)contextV;
    (void)key;
    (void)keySz;

    return NOT_COMPILED_IN;
#endif
}


/* Perform XOR encryption over the first parameter of a TPM packet */
static int TPM2_ParamEnc_XOR(TPM2_AUTH_SESSION *session, TPM2B_AUTH* keyIn,
    TPM2B_NONCE* nonceCaller, TPM2B_NONCE* nonceTPM, BYTE *paramData, 
    UINT32 paramSz)
{
    int rc = TPM_RC_FAILURE;
    TPM2B_MAX_BUFFER mask;
    UINT32 i;

    if (paramSz > sizeof(mask.buffer)) {
        return BUFFER_E;
    }

    /* Generate XOR Mask stream matching paramater size */
    XMEMSET(mask.buffer, 0, sizeof(mask.buffer));
    rc = TPM2_KDFa(session->authHash, (TPM2B_DATA*)keyIn, "XOR",
        nonceCaller, nonceTPM, mask.buffer, paramSz);
    if ((UINT32)rc != paramSz) {
    #ifdef DEBUG_WOLFTPM
        printf("KDFa XOR Gen Error %d\n", rc);
    #endif
        return TPM_RC_FAILURE;
    }

    /* Perform XOR */
    for (i = 0; i < paramSz; i++) {
        paramData[i] = paramData[i] ^ mask.buffer[i];
    }

    /* Data size matched and data encryption completed at this point */
    rc = TPM_RC_SUCCESS;

    return rc;
}

/* Perform XOR decryption over the first parameter of a TPM packet */
static int TPM2_ParamDec_XOR(TPM2_AUTH_SESSION *session, TPM2B_AUTH* keyIn,
    TPM2B_NONCE* nonceCaller, TPM2B_NONCE* nonceTPM, BYTE *paramData, 
    UINT32 paramSz)
{
    int rc = TPM_RC_FAILURE;
    TPM2B_MAX_BUFFER mask;
    UINT32 i;

    if (paramSz > sizeof(mask.buffer)) {
        return BUFFER_E;
    }

    /* Generate XOR Mask stream matching paramater size */
    XMEMSET(mask.buffer, 0, sizeof(mask.buffer));
    rc = TPM2_KDFa(session->authHash, (TPM2B_DATA*)keyIn, "XOR", 
        nonceTPM, nonceCaller, mask.buffer, paramSz);
    if ((UINT32)rc != paramSz) {
    #ifdef DEBUG_WOLFTPM
        printf("KDFa XOR Gen Error %d\n", rc);
    #endif
        return TPM_RC_FAILURE;
    }

    /* Perform XOR */
    for (i = 0; i < paramSz; i++) {
        paramData[i] = paramData[i] ^ mask.buffer[i];
    }
    /* Data size matched and data encryption completed at this point */
    rc = TPM_RC_SUCCESS;

    return rc;
}

#ifdef WOLFSSL_AES_CFB
/* Perform AES CFB encryption over the first parameter of a TPM packet */
static int TPM2_ParamEnc_AESCFB(TPM2_AUTH_SESSION *session, TPM2B_AUTH* keyIn,
    TPM2B_NONCE* nonceCaller, TPM2B_NONCE* nonceTPM, BYTE *paramData, 
    UINT32 paramSz)
{
    int rc = TPM_RC_FAILURE;
    BYTE symKey[32 + 16]; /* AES key (max) + IV (block size) */
    int symKeySz = session->symmetric.keyBits.aes / 8;
    const int symKeyIvSz = 16;
    Aes enc;

    if (symKeySz > 32) {
        return BUFFER_E;
    }

    /* Generate AES Key and IV */
    XMEMSET(symKey, 0, sizeof(symKey));
    rc = TPM2_KDFa(session->authHash, (TPM2B_DATA*)keyIn, "CFB",
        nonceCaller, nonceTPM, symKey, symKeySz + symKeyIvSz);
    if (rc != symKeySz + symKeyIvSz) {
    #ifdef DEBUG_WOLFTPM
        printf("KDFa CFB Gen Error %d\n", rc);
    #endif
        return TPM_RC_FAILURE;
    }

#ifdef WOLFTPM_DEBUG_VERBOSE
    printf("AES Enc Key %d, IV %d\n", symKeySz, symKeyIvSz);
    TPM2_PrintBin(symKey, symKeySz);
    TPM2_PrintBin(&symKey[symKeySz], symKeyIvSz);
#endif

    /* Perform AES CFB Encryption */
    rc = wc_AesInit(&enc, NULL, INVALID_DEVID);
    if (rc == 0) {
        rc = wc_AesSetKey(&enc, symKey, symKeySz, &symKey[symKeySz], AES_ENCRYPTION);
        if (rc == 0) {
            rc = wc_AesCfbEncrypt(&enc, paramData, paramData, paramSz);
        }
        wc_AesFree(&enc);
    }

    return rc;
}

/* Perform AES CFB decryption over the first parameter of a TPM packet */
static int TPM2_ParamDec_AESCFB(TPM2_AUTH_SESSION *session, TPM2B_AUTH* keyIn,
    TPM2B_NONCE* nonceCaller, TPM2B_NONCE* nonceTPM, BYTE *paramData, 
    UINT32 paramSz)
{
    int rc = TPM_RC_FAILURE;
    BYTE symKey[32 + 16];	/* AES key 128-bit + IV (block size) */
    int symKeySz = session->symmetric.keyBits.aes / 8;
    const int symKeyIvSz = 16;
    Aes dec;

    if (symKeySz > 32) {
        return BUFFER_E;
    }

    /* Generate AES Key and IV */
    XMEMSET(symKey, 0, sizeof(symKey));
    rc = TPM2_KDFa(session->authHash, (TPM2B_DATA*)keyIn, "CFB",
        nonceTPM, nonceCaller, symKey, symKeySz + symKeyIvSz);
    if (rc != symKeySz + symKeyIvSz) {
    #ifdef DEBUG_WOLFTPM
        printf("KDFa CFB Gen Error %d\n", rc);
    #endif
        return TPM_RC_FAILURE;
    }

#ifdef WOLFTPM_DEBUG_VERBOSE
    printf("AES Dec Key %d, IV %d\n", symKeySz, symKeyIvSz);
    TPM2_PrintBin(symKey, symKeySz);
    TPM2_PrintBin(&symKey[symKeySz], symKeyIvSz);
#endif

    /* Perform AES CFB Decryption */
    rc = wc_AesInit(&dec, NULL, INVALID_DEVID);
    if (rc == 0) {
        rc = wc_AesSetKey(&dec, symKey, symKeySz, &symKey[symKeySz], AES_DECRYPTION);
        if (rc == 0) {
            rc = wc_AesCfbDecrypt(&dec, paramData, paramData, paramSz);
        }
        wc_AesFree(&dec);
    }

    return rc;
}
#endif

/******************************************************************************/
/* --- Public Functions -- */
/******************************************************************************/

TPM_RC TPM2_ParamEnc_CmdRequest(TPM2_AUTH_SESSION *session,
                                BYTE *paramData, UINT32 paramSz)
{
    TPM_RC rc = TPM_RC_FAILURE;

#ifdef WOLFTPM_DEBUG_VERBOSE
    printf("CmdEnc Session Key %d\n", session->auth.size);
	TPM2_PrintBin(session->auth.buffer, session->auth.size);
    printf("CmdEnc Nonce caller %d\n", session->nonceCaller.size);
    TPM2_PrintBin(session->nonceCaller.buffer, session->nonceCaller.size);
    printf("CmdEnc Nonce TPM %d\n", session->nonceTPM.size);
    TPM2_PrintBin(session->nonceTPM.buffer, session->nonceTPM.size);
#endif

    if (session->symmetric.algorithm == TPM_ALG_XOR) {
        rc = TPM2_ParamEnc_XOR(session, &session->auth, &session->nonceCaller,
            &session->nonceTPM, paramData, paramSz);
    }
#ifdef WOLFSSL_AES_CFB
    else if (session->symmetric.algorithm == TPM_ALG_AES && 
             session->symmetric.mode.aes == TPM_ALG_CFB) {
        rc = TPM2_ParamEnc_AESCFB(session, &session->auth, &session->nonceCaller,
            &session->nonceTPM, paramData, paramSz);
    }
#endif

    return rc;
}

TPM_RC TPM2_ParamDec_CmdResponse(TPM2_AUTH_SESSION *session,
                                 BYTE *paramData, UINT32 paramSz)
{
    TPM_RC rc = TPM_RC_FAILURE;

#ifdef WOLFTPM_DEBUG_VERBOSE
    printf("RspDec Session Key %d\n", session->auth.size);
	TPM2_PrintBin(session->auth.buffer, session->auth.size);
    printf("RspDec Nonce caller %d\n", session->nonceCaller.size);
    TPM2_PrintBin(session->nonceCaller.buffer, session->nonceCaller.size);
    printf("RspDec Nonce TPM %d\n", session->nonceTPM.size);
    TPM2_PrintBin(session->nonceTPM.buffer, session->nonceTPM.size);
#endif

    if (session->symmetric.algorithm == TPM_ALG_XOR) {
        rc = TPM2_ParamDec_XOR(session, &session->auth, &session->nonceCaller,
            &session->nonceTPM, paramData, paramSz);
    }
#ifdef WOLFSSL_AES_CFB
    else if (session->symmetric.algorithm == TPM_ALG_AES &&
             session->symmetric.mode.aes == TPM_ALG_CFB) {
        rc = TPM2_ParamDec_AESCFB(session, &session->auth, &session->nonceCaller,
            &session->nonceTPM, paramData, paramSz);
    }
#endif

    return rc;
}
