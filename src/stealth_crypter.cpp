// Copyright (c) 2014 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Stealth-address narration crypter implementation (rehomed from the retired
// SMSG/NYX messaging subsystem; see stealth_crypter.h). Behaviour is unchanged.
#include "stealth_crypter.h"

#include "allocators.h"

#include <string.h>

#include <openssl/aes.h>
#include <openssl/evp.h>

SecMsgCrypter::SecMsgCrypter()
{
    // Try to keep the key data out of swap (and be a bit over-careful to keep the IV that we don't even use out of swap)
    // Note that this does nothing about suspend-to-disk (which will put all our key data on disk)
    // Note as well that at no point in this program is any attempt made to prevent stealing of keys by reading the memory of the running process.
    LockedPageManager::instance.LockRange(&chKey[0], sizeof chKey);
    LockedPageManager::instance.LockRange(&chIV[0], sizeof chIV);
    fKeySet = false;
}

SecMsgCrypter::~SecMsgCrypter()
{
    // clean key
    memset(&chKey, 0, sizeof chKey);
    memset(&chIV, 0, sizeof chIV);
    fKeySet = false;

    LockedPageManager::instance.UnlockRange(&chKey[0], sizeof chKey);
    LockedPageManager::instance.UnlockRange(&chIV[0], sizeof chIV);
}

bool SecMsgCrypter::SetKey(const std::vector<unsigned char>& vchNewKey, unsigned char* chNewIV)
{
    if (vchNewKey.size() < sizeof(chKey))
        return false;

    return SetKey(&vchNewKey[0], chNewIV);
}

bool SecMsgCrypter::SetKey(const unsigned char* chNewKey, unsigned char* chNewIV)
{
    // -- for EVP_aes_256_cbc() key must be 256 bit, iv must be 128 bit.
    memcpy(&chKey[0], chNewKey, sizeof(chKey));
    memcpy(chIV, chNewIV, sizeof(chIV));

    fKeySet = true;
    return true;
}

bool SecMsgCrypter::Encrypt(unsigned char* chPlaintext, uint32_t nPlain, std::vector<unsigned char> &vchCiphertext)
{
    if (!fKeySet)
        return false;

    // -- max ciphertext len for a n bytes of plaintext is n + AES_BLOCK_SIZE - 1 bytes
    int nLen = nPlain;

    int nCLen = nLen + AES_BLOCK_SIZE, nFLen = 0;
    vchCiphertext = std::vector<unsigned char> (nCLen);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;

    bool fOk = true;
    if (fOk) fOk = EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, &chKey[0], &chIV[0]);
    if (fOk) fOk = EVP_EncryptUpdate(ctx, &vchCiphertext[0], &nCLen, chPlaintext, nLen);
    if (fOk) fOk = EVP_EncryptFinal_ex(ctx, (&vchCiphertext[0])+nCLen, &nFLen);
    EVP_CIPHER_CTX_free(ctx);

    if (!fOk)
        return false;

    vchCiphertext.resize(nCLen + nFLen);

    return true;
}

bool SecMsgCrypter::Decrypt(unsigned char* chCiphertext, uint32_t nCipher, std::vector<unsigned char>& vchPlaintext)
{
    if (!fKeySet)
        return false;

    // plaintext will always be equal to or lesser than length of ciphertext
    int nPLen = nCipher, nFLen = 0;

    vchPlaintext.resize(nCipher);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;

    bool fOk = true;
    if (fOk) fOk = EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, &chKey[0], &chIV[0]);
    if (fOk) fOk = EVP_DecryptUpdate(ctx, &vchPlaintext[0], &nPLen, &chCiphertext[0], nCipher);
    if (fOk) fOk = EVP_DecryptFinal_ex(ctx, (&vchPlaintext[0])+nPLen, &nFLen);
    EVP_CIPHER_CTX_free(ctx);

    if (!fOk)
        return false;

    vchPlaintext.resize(nPLen + nFLen);

    return true;
}
